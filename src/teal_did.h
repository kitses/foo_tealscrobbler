#pragma once

#include "json.hpp"

#include <string>

namespace teal::detail
{

using json = nlohmann::json;

std::string did_document_url(const std::string &did);
std::string extract_pds(const json &document);

} // namespace teal::detail
