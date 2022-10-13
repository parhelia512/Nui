#pragma once

#include <nui/frontend/attributes/impl/attribute.hpp>
#include <nui/frontend/event_system/observed_value.hpp>
#include <nui/frontend/event_system/observed_value_combinator.hpp>
#include <nui/utility/meta/is_tuple.hpp>

#include <utility>
#include <tuple>
#include <optional>
#include <string>

namespace Nui
{
    template <typename T, typename Tag>
    class CustomAttribute
    {
      public:
        template <typename U = T>
        CustomAttribute(U&& param)
            : param_(std::forward<U>(param))
        {}
        T get() const&
        {
            return param_;
        }
        T&& get() &&
        {
            return std::move(param_);
        }

      private:
        T param_;
    };

    namespace Detail
    {
        class InvalidAttribute
        {
          public:
            InvalidAttribute get() const
            {
                return {};
            }
        };

        template <typename Tag, typename T>
        struct IsAttributeOfTag
        {
            static constexpr bool value = false;
        };
        template <typename Tag, typename T>
        struct IsAttributeOfTag<Tag, CustomAttribute<T, Tag>>
        {
            static constexpr bool value = true;
        };

        template <typename Tag, typename... Attributes>
        constexpr static auto* extractAttributeImpl()
        {
            return static_cast<InvalidAttribute*>(nullptr);
        }

        template <typename Tag, typename Front, typename... Attributes>
        constexpr static auto* extractAttributeImpl(Front& front, Attributes&... attributes)
        {
            if constexpr (IsAttributeOfTag<Tag, Front>::value)
            {
                return &front;
            }
            else
            {
                return extractAttributeImpl<Tag>(attributes...);
            }
        }
    }

    template <typename Tag, typename... Attributes>
    constexpr auto* extractOptionalAttributePtr(Attributes&... attributes)
    {
        return Detail::extractAttributeImpl<Tag>(attributes...);
    }

    /**
     * @brief This function takes less compiler effort if you pass the expected output type.
     */
    template <typename Tag, typename ExpectedType, typename... Attributes>
    constexpr std::optional<ExpectedType> extractOptionalAttributeFast(Attributes&... attributes)
    {
        auto attribute = extractOptionalAttributePtr<Tag>(attributes...);
        if constexpr (std::is_same_v<Detail::InvalidAttribute*, decltype(attribute)>)
            return std::nullopt;
        else
            return attribute->get();
    }

    template <typename Tag, typename... Attributes>
    constexpr auto extractAttributeAsTuple(Attributes&... attributes)
    {
        auto attribute = extractOptionalAttributePtr<Tag>(attributes...);
        if constexpr (std::is_same_v<Detail::InvalidAttribute*, decltype(attribute)>)
            return std::tuple<>{};
        else if constexpr (IsTuple_v<decltype(attribute->get())>)
            return attribute->get();
        else
            return std::make_tuple(attribute->get());
    }

    template <typename Tag, typename... Attributes>
    constexpr auto extractOptionalAttribute(Attributes&... attributes)
    {
        return extractOptionalAttributeFast<
            Tag,
            std::decay_t<decltype(extractOptionalAttributePtr<Tag>(attributes...)->get())>>(attributes...);
    }

    template <typename Tag, typename... Attributes>
    constexpr auto extractAttribute(Attributes&... attributes)
    {
        auto attr = extractOptionalAttributePtr<Tag>(attributes...);
        // If you land here, a component expects an attribute to be passed, but didnt.
        static_assert(
            !std::is_same_v<Detail::InvalidAttribute*, decltype(attr)>,
            "Attribute is required. Look for a 'extractAttribute<TAG_HERE>' in the error log");
        return attr->get();
    }

#define NUI_MAKE_CUSTOM_ATTRIBUTE(NAME) \
    struct NAME##Tag \
    { \
        template <typename T> \
        std::enable_if_t<!::Nui::Detail::IsObserved_v<std::decay_t<T>>, CustomAttribute<T, NAME##Tag>> \
        operator=(T&& param) const \
        { \
            return std::forward<T>(param); \
        } \
        template <typename T> \
        std::enable_if_t<::Nui::Detail::IsObserved_v<std::decay_t<T>>, CustomAttribute<T&, NAME##Tag>> \
        operator=(T& param) const \
        { \
            return {param}; \
        } \
        template <typename RendererType, typename... ObservedValues> \
        CustomAttribute<ObservedValueCombinatorWithGenerator<RendererType, ObservedValues...>, NAME##Tag> \
        operator=(ObservedValueCombinatorWithGenerator<RendererType, ObservedValues...> const& combinator) const \
        { \
            return CustomAttribute<ObservedValueCombinatorWithGenerator<RendererType, ObservedValues...>, NAME##Tag>{ \
                combinator}; \
        } \
    } static constexpr NAME
}