#pragma once

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <helpers/logger.hpp>
#include <optional>
#include <server/moonlight_proto.hpp>
#include <sstream>
#include <string>

using XML = moonlight::XML;
namespace pt = boost::property_tree;

inline std::string xml_to_str(const XML &xml) {
  std::stringstream ss;
  pt::write_xml(ss, xml);
  return ss.str();
}

template <class T> inline void log_req(const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> &request) {
  logs::log(logs::debug,
            "{} [{}] {}{}",
            request->remote_endpoint().address().to_string(),
            request->method,
            std::is_same_v<SimpleWeb::HTTP, T> ? "HTTP://" : "HTTPS://",
            request->path);
}

template <class T>
inline void send_xml(const std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> &response,
                     SimpleWeb::StatusCode status_code,
                     const XML &xml) {
  std::ostringstream data;
  pt::write_xml(data, xml);
  logs::log(logs::trace, "Response: {}", data.str());
  response->write(status_code, data.str());
  response->close_connection_after_response = true;
}

inline std::optional<std::string> get_header(const SimpleWeb::CaseInsensitiveMultimap &headers, const std::string &key) {
  auto it = headers.find(key);
  if (it != headers.end())
    return it->second;
  return {};
}

inline XML fail_pair(const std::string &status_msg) {
  logs::log(logs::warning, "Failed pairing: {}", status_msg);
  XML tree;
  tree.put("root.paired", 0);
  tree.put("root.<xmlattr>.status_code", 400);
  tree.put("root.<xmlattr>.status_message", status_msg);
  return tree;
}
