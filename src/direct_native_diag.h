#pragma once
#include <string>
// Probe-only alternatives from the public uMediaClient URI envelope. They are
// hypotheses for Starfish 3.x, not a claim that its private API accepts them.
enum class DirectLoadContract { ArgsHls, UriHls, UriAuto };
const char *direct_contract_name(DirectLoadContract);
DirectLoadContract direct_configured_contract();
const char *direct_native_event_name(int);
bool direct_native_event_has_text(int);
std::string direct_native_json_summary(const char *message);
bool direct_fixture_uri_valid(const std::string& uri,const std::string& lan);
