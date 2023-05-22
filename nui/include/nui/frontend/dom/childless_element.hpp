#pragma once

#include <nui/frontend/dom/basic_element.hpp>
#include <nui/frontend/elements/impl/html_element.hpp>
#include <nui/frontend/utility/functions.hpp>

namespace Nui::Dom
{
    /**
     * @brief The basic element cannot have children and does not hold explicit ownership of them.
     * To represent an actual HtmlElement use the Element class.
     */
    class ChildlessElement : public BasicElement
    {
      public:
        ChildlessElement(HtmlElement const& elem)
            : BasicElement{ChildlessElement::createElement(elem).val()}
        {}
        ChildlessElement(emscripten::val val)
            : BasicElement{std::move(val)}
        {}

        // TODO: more overloads?
        void setAttribute(std::string_view key, std::string const& value)
        {
            // FIXME: performance, keys are turned to std::string
            if (value.empty())
                element_.call<emscripten::val>("removeAttribute", emscripten::val{std::string{key}});
            else
                element_.call<emscripten::val>(
                    "setAttribute", emscripten::val{std::string{key}}, emscripten::val{value});
        }
        void setAttribute(std::string_view key, std::invocable<emscripten::val> auto&& value)
        {
            element_.set(emscripten::val{std::string{key}}, Nui::bind(value, std::placeholders::_1));
        }
        void setAttribute(std::string_view key, char const* value)
        {
            if (value[0] == '\0')
                element_.call<emscripten::val>("removeAttribute", emscripten::val{std::string{key}});
            else
                element_.call<emscripten::val>(
                    "setAttribute", emscripten::val{std::string{key}}, emscripten::val{std::string{value}});
        }
        void setAttribute(std::string_view key, bool value)
        {
            if (value)
                element_.call<emscripten::val>("setAttribute", emscripten::val{std::string{key}}, emscripten::val{""});
        }
        void setAttribute(std::string_view key, int value)
        {
            element_.call<emscripten::val>("setAttribute", emscripten::val{std::string{key}}, emscripten::val{value});
        }
        void setAttribute(std::string_view key, double value)
        {
            element_.call<emscripten::val>("setAttribute", emscripten::val{std::string{key}}, emscripten::val{value});
        }
        void setAttribute(std::string_view key, emscripten::val value)
        {
            element_.call<emscripten::val>("setAttribute", emscripten::val{std::string{key}}, value);
        }
        template <typename T>
        void setAttribute(std::string_view key, std::optional<T> const& value)
        {
            if (value)
                setAttribute(key, *value);
        }

      protected:
        static ChildlessElement createElement(HtmlElement const& element)
        {
            return {emscripten::val::global("document")
                        .call<emscripten::val>("createElement", emscripten::val{element.name()})};
        }
    };
};
