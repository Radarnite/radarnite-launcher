#pragma once

// fnoffline: Discord webhooks (libcurl) removed. This stub keeps the same
// interface but never touches the network; every call is a no-op.

#include <string>

class DiscordWebhook {
public:
    DiscordWebhook(const char*) {}

    inline bool send_message(const std::string&) { return false; }
    inline bool send_embedjson(const std::string) { return false; }
    inline bool send_embed(const std::string&, const std::string&, int = 0) { return false; }
};

namespace Information
{
    static std::string UptimeWebHook = ("");
}

static DiscordWebhook UptimeWebHook(Information::UptimeWebHook.c_str());
