// 5G-MAG Reference Tools
// MBMS Middleware Process
//
// Copyright (C) 2021 Daniel Silhavy (Fraunhofer FOKUS)
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

#include "DashManifest.h"

#include "tinyxml2.h"
#include <utility>
#include <iomanip>      // std::get_time
#include <chrono>
#include <sstream>
#include "spdlog/spdlog.h"
#include "Poco/URI.h"

MBMS_RT::DashManifest::DashManifest(const std::string& content, size_t time_offset)
{
  tinyxml2::XMLDocument doc;
  doc.Parse(content.c_str());

  // strip the protocol and server from our version of the MPD, so the player
  // loads resources from relative paths
  auto* mpd = doc.FirstChildElement("MPD");
  auto* base_url_element = mpd->FirstChildElement("BaseURL");
  Poco::URI base_url(base_url_element->GetText());
  base_url_element->SetText(base_url.getPathEtc().c_str());

  std::stringstream  availability_start_time(mpd->Attribute("availabilityStartTime"));
  spdlog::info("ast {}, time offset {}", availability_start_time.str() , time_offset);

  struct std::tm ast;
  availability_start_time >> std::get_time(&ast, "%Y-%m-%dT%H:%M:%SZ");
  auto ast_tm = timegm(&ast);

  spdlog::info("parsed: {}", ast_tm);
  ast_tm += static_cast<time_t>(time_offset/1000);
  spdlog::info("adjusted: {}", ast_tm);
  std::array<char,21> buff;
  strftime(buff.data(), buff.size(), "%Y-%m-%dT%H:%M:%SZ", gmtime(&ast_tm));
  spdlog::info("formatted: {}", buff.data());
  mpd->SetAttribute("availabilityStartTime", buff.data());

  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);

  _content = printer.CStr();

  spdlog::info("MPD base url: {}", _content);
}

