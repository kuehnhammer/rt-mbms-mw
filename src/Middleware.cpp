// 5G-MAG Reference Tools
// MBMS Middleware Process
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
// 
// See the License for the specific language governing permissions and limitations
// under the License.
//
#include "Middleware.h"
#include "spdlog/spdlog.h"
#include <boost/bind/bind.hpp>

using namespace boost::placeholders;

/**
 *
 * @param io_service
 * @param cfg
 * @param api_url
 * @param iface
 */
MBMS_RT::Middleware::Middleware(boost::asio::io_service &io_service, 
    const libconfig::Config &cfg, //NOLINT
    const std::string &api_url, // NOLINT
    const std::string &iface) // NOLINT
    : _cache(cfg, io_service)
    , _api(cfg, api_url, _cache, &_service_announcement, _services)
    , _tick_interval(1)
    , _timer(io_service, _tick_interval)
    , _cfg(cfg)
    , _interface(iface)
    , _io_service(io_service)
{
  cfg.lookupValue("mw.seamless_switching.enabled", _seamless);
  if (_seamless) {
    spdlog::info("Seamless switching mode enabled");
  }

  if (!_handle_local_service_announcement()) {
    std::string sa_mcast = _sa_address + ":" + std::to_string(_sa_port);
    std::string pcap_file = "";

    if(cfg.lookup("mw.pcap_input.enabled")) {
      pcap_file = (std::string)cfg.lookup("mw.pcap_input.pcap_file");
    }

    _service_announcement = std::make_unique<MBMS_RT::ServiceAnnouncement>(
        _cfg, sa_mcast, _sa_tsi, _interface,
        _io_service,
        _cache, _seamless,
        [&](const std::string& service_id) { return get_service(service_id); },
        [&](const std::string& service_id, std::shared_ptr<Service> service) 
        { set_service(service_id, std::move(service)); },
        pcap_file); 

    _service_announcement->start_flute_receiver(sa_mcast);
  }

  _timer.async_wait(boost::bind(&Middleware::tick_handler, this)); //NOLINT
}

/**
 *
 * @return {bool} Whether a local SA file was used
 */
auto MBMS_RT::Middleware::_handle_local_service_announcement() -> bool {
  try {
    bool local_service_enabled = false;
    std::string local_bootstrap_file;

    _cfg.lookupValue("mw.local_service.enabled", local_service_enabled);
    _cfg.lookupValue("mw.local_service.bootstrap_file", local_bootstrap_file);

    if (local_service_enabled && local_bootstrap_file != "") {
      spdlog::info("Reading service announcement from file at {}", local_bootstrap_file);
      std::ifstream ifs(local_bootstrap_file);
      std::string sa_multipart((std::istreambuf_iterator<char>(ifs)),
                               (std::istreambuf_iterator<char>()));
      std::string mcast_address = "";
      _cfg.lookupValue("mw.local_service.mcast_address", mcast_address);

      _service_announcement = std::make_unique<MBMS_RT::ServiceAnnouncement>(
          _cfg, mcast_address, 0, _interface,
          _io_service, _cache, _seamless,
        [&](const std::string& service_id) { return get_service(service_id); },
        [&](const std::string& service_id, std::shared_ptr<Service> service)
          { set_service(service_id, std::move(service)); });

      _service_announcement->parse_bootstrap(sa_multipart);

      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

/**
 *
 */
void MBMS_RT::Middleware::tick_handler() {
  _cache.check_file_expiry_and_cache_size();

  _timer.expires_at(_timer.expires_at() + _tick_interval);
  _timer.async_wait(boost::bind(&Middleware::tick_handler, this)); //NOLINT
}

/**
 *
 * @param {string} service_id
 * @return
 */
auto MBMS_RT::Middleware::get_service(const std::string &service_id) -> std::shared_ptr<Service> {
  if (_services.find(service_id) != _services.end()) {
    return _services[service_id];
  } else {
    return nullptr;
  }
}
