#include <nui/frontend/utility/fragment_listener.hpp>

#include <nui/frontend/utility/functions.hpp>
#include <nui/frontend/api/console.hpp>
#include <nui/frontend/event_system/event_context.hpp>

#include <emscripten/val.h>

using namespace std::string_literals;

namespace Nui
{
    void listenToFragmentChanges(Observed<std::string>& fragment)
    {
        emscripten::val::global("window").call<void>(
            "addEventListener",
            "hashchange"s,
            Nui::bind(
                [&fragment](emscripten::val event) {
                    auto movedTo = event["newURL"].as<std::string>();
                    auto hashPos = movedTo.find_last_of('#');
                    if (hashPos != std::string::npos)
                        fragment = movedTo.substr(hashPos + 1);
                    else
                        fragment = "";
                    globalEventContext.executeActiveEventsImmediately();
                },
                std::placeholders::_1));
    }
}
