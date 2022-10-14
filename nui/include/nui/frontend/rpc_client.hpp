#pragma once

#include <emscripten/val.h>

#include <nui/frontend/api/console.hpp>
#include <nui/frontend/utility/functions.hpp>

#include <string>

namespace Nui
{
    class RpcClient
    {
      public:
        class RemoteCallable
        {
          public:
            template <typename... Args>
            auto operator()(Args&&... args) const
            {
                using namespace std::string_literals;
                if (!resolve())
                {
                    Console::error("Remote callable with name '"s + name_ + "' is undefined");
                    return emscripten::val::undefined();
                }
                return callable_(emscripten::val{args}...);
            }
            auto operator()(emscripten::val val) const
            {
                using namespace std::string_literals;
                if (!resolve())
                {
                    Console::error("Remote callable with name '"s + name_ + "' is undefined");
                    return emscripten::val::undefined();
                }
                return callable_(val);
            }

            RemoteCallable(std::string name)
                : name_{std::move(name)}
                , callable_{emscripten::val::undefined()}
                , isSet_{false}
            {}

          private:
            bool resolve() const
            {
                using namespace std::string_literals;
                if (isSet_)
                    return true;

                const auto rpcObject = emscripten::val::global("nui_rpc");
                if (rpcObject.typeOf().as<std::string>() == "undefined")
                    return false;

                callable_ = emscripten::val::global("nui_rpc")["backend"][name_.c_str()];
                isSet_ = callable_.typeOf().as<std::string>() != "undefined";
                return isSet_;
            }

          private:
            std::string name_;
            mutable emscripten::val callable_;
            mutable bool isSet_;
        };

        /**
         * @brief Get a callable remote function.
         *
         * @param name Name of the function.
         * @return auto A wrapper that can be called.
         */
        static auto getRemoteCallable(std::string name)
        {
            using namespace std::string_literals;
            return RemoteCallable{std::move(name)};
        }

        /**
         * @brief Registers a single shot function that is removed after it was called.
         *
         * @param func The function to call.
         * @return std::string The generated name of the function.
         */
        template <typename FunctionT>
        static std::string registerFunctionOnce(FunctionT&& func)
        {
            using namespace std::string_literals;
            if (emscripten::val::global("nui_rpc").typeOf().as<std::string>() == "undefined")
            {
                Console::error("rpc was not setup by backend"s);
                return {};
            }
            auto tempId = emscripten::val::global("nui_rpc")["tempId"].as<int>();
            ++tempId;
            auto tempIdVal = emscripten::val::global("nui_rpc")["tempId"];
            tempIdVal = emscripten::val{tempId};
            const auto tempIdString = "temp_"s + std::to_string(tempId);
            emscripten::val::global("nui_rpc")["frontend"].set(
                tempIdString,
                Nui::bind(
                    [func = std::move(func)](emscripten::val param) {
                        func(param);
                        emscripten::val::global("nui_rpc")["frontend"].set(
                            "temp_"s + param.as<std::string>(), emscripten::val::undefined());
                    },
                    std::placeholders::_1));
            return tempIdString;
        }

        /**
         * @brief Register a permanent function that is callable from the backend.
         *
         * @param name The name of the function.
         * @param func The function itself.
         */
        template <typename FunctionT>
        static void registerFunction(std::string const& name, FunctionT&& func)
        {
            using namespace std::string_literals;
            if (emscripten::val::global("nui_rpc").typeOf().as<std::string>() == "undefined")
            {
                Console::error("rpc was not setup by backend"s);
                return;
            }
            emscripten::val::global("nui_rpc")["frontend"].set(
                (name).c_str(),
                Nui::bind(
                    [func = std::move(func)](emscripten::val param) {
                        func(param);
                    },
                    std::placeholders::_1));
        }
    };
}