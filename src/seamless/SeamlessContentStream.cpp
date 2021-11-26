// OBECA - Open Broadcast Edge Cache Appliance
// Gateway Process
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <regex>
#include "SeamlessContentStream.h"
#include "CdnClient.h"
#include "CacheItems.h"
#include "HlsMediaPlaylist.h"
#include <libgen.h>

#include "spdlog/spdlog.h"
#include "cpprest/base_uri.h"

MBMS_RT::SeamlessContentStream::SeamlessContentStream(std::string base, std::string flute_if, boost::asio::io_service& io_service, CacheManagement& cache, DeliveryProtocol protocol, const libconfig::Config& cfg)
  : ContentStream( std::move(base), std::move(flute_if), io_service, cache, protocol, cfg)
  , _tick_interval(1)
  , _timer(io_service, _tick_interval)
{
  cfg.lookupValue("mw.cache.max_segments_per_stream", _segments_to_keep);
  cfg.lookupValue("mw.seamless_switching.truncate_cdn_playlist_segments", _truncate_cdn_playlist_segments);
  _timer.async_wait(boost::bind(&SeamlessContentStream::tick_handler, this)); //NOLINT
}

auto MBMS_RT::SeamlessContentStream::flute_file_received(std::shared_ptr<LibFlute::File> file) -> void {
  spdlog::debug("SeamlessContentStream: {} (TOI {}, MIME type {}) has been received",
      file->meta().content_location, file->meta().toi, file->meta().content_type);

  if (file->meta().content_location == _playlist_path) {
    spdlog::info("ContentStream: got PLAYLIST at {}", file->meta().content_location);
    handle_playlist(std::string(file->buffer(), file->length()), MBMS_RT::ItemSource::Broadcast);
  } else if (file->meta().content_location == "index.m3u8") {
    // ignore the pathless master manifest generated by the core
  } else {
    spdlog::info("ContentStream: got SEGMENT at {}", file->meta().content_location);
    _flute_files[file->meta().content_location] = file;
  }
}

auto MBMS_RT::SeamlessContentStream::set_cdn_endpoint(const std::string& cdn_ept) -> void
{
  web::uri uri(cdn_ept);
  web::uri_builder cdn_base(cdn_ept);

  std::string path = uri.path();
  _playlist_path = path.erase(0,1) + "?" + uri.query();
  spdlog::debug("ContentStream: playlist location is {}", _playlist_path);
  size_t spos = _playlist_path.rfind('/');
  _playlist_dir = _playlist_path.substr(0, spos+1);
  spdlog::debug("ContentStream: playlist dir is {}", _playlist_dir);

  cdn_base.set_path("");
  cdn_base.set_query("");
  _cdn_endpoint = cdn_base.to_string(); 
  spdlog::info("ContentStream: setting CDN ept to {}", _cdn_endpoint);
  
  _cdn_client = std::make_shared<CdnClient>(_cdn_endpoint);

  _cache.add_item( std::make_shared<CachedPlaylist>(
        _playlist_path,
        0,
        [&]() -> const std::string& {
          spdlog::info("ContentStream: {} playlist requested", _playlist_path);
          return _playlist;
        }
        ));
};


auto MBMS_RT::SeamlessContentStream::handle_playlist( const std::string& content, ItemSource source) -> void
{
  auto playlist = MBMS_RT::HlsMediaPlaylist(content);
  int seq = 0;
  double extinf = 0;

  auto count = playlist.segments().size();
  if (source == ItemSource::CDN) {
    count -= _truncate_cdn_playlist_segments;
  }
  int idx = 0;

  const std::lock_guard<std::mutex> lock(_segments_mutex);
  for (const auto& segment : playlist.segments()) {
    spdlog::debug("segment: seq {}, extinf {}, uri {}", segment.seq, segment.extinf, segment.uri);
    if (_segments.find(segment.seq) == _segments.end()) {
      std::string full_uri = _playlist_dir + segment.uri;
      auto seg =
        std::make_shared<Segment>(full_uri, segment.seq, segment.extinf);
      if (_cdn_client) {
        seg->set_cdn_client(_cdn_client);
      }

      if (_flute_files.find(full_uri) != _flute_files.end()) {
        seg->set_flute_file(_flute_files[full_uri]);
        _flute_files.erase(full_uri);
        spdlog::debug("Assigned already received flute file");
      }

      _segments[segment.seq] = seg;

      _cache.add_item( std::make_shared<CachedSegment>(
            full_uri, 0, seg )
          );
    }
    if (idx++ > count) {
      break;
    }
  }

  while (_segments.size() > _segments_to_keep) {
    auto seg = _segments.extract(_segments.begin());
    spdlog::debug("Removing oldest segment and cache item at {}", seg.mapped()->uri());
    _cache.remove_item(seg.mapped()->uri());
  }
  HlsMediaPlaylist pl;
  pl.set_target_duration(playlist.target_duration());  // [TODO] this will fail when targetdurations change or do not match
  for (const auto& seg : _segments) {
    HlsMediaPlaylist::Segment s{
      seg.second->uri(),
      seg.second->seq(),
      seg.second->extinf(),
    };
    pl.add_segment(s);
  }
  _playlist = pl.to_string();
}

auto MBMS_RT::SeamlessContentStream::tick_handler() -> void
{
  spdlog::debug("Getting playlist from CDN at {}", _playlist_path);
  if (_cdn_client) {
    _cdn_client->get(_playlist_path,
        [&](std::shared_ptr<CdnFile> file) -> void { //NOLINT
        spdlog::debug("Playlist received from CDN");
            handle_playlist(std::string(file->buffer(), file->length()), MBMS_RT::ItemSource::CDN);
        });
  }
  _timer.expires_at(_timer.expires_at() + _tick_interval);
  _timer.async_wait(boost::bind(&SeamlessContentStream::tick_handler, this)); //NOLINT
}

