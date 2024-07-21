namespace Nui::Detail
{
    enum RangeStillValid : bool
    {
        InvalidateRange = false,
        KeepRange = true,
    };

    template <typename ObservedType>
    struct HoldToken
    {
        // FIXME: Remove this, when getValueRange overload is not instantiated for non weak observed types
        std::shared_ptr<ObservedType> locked;
    };

    template <typename T>
    struct HoldToken<std::weak_ptr<T>>
    {
        std::shared_ptr<T> locked;
    };

    template <typename T>
    using CommonHoldToken = ::Nui::Detail::HoldToken<ObservedAddMutableReference_raw<typename T::ObservedType>>;

    template <typename RangeT, typename GeneratorT>
    class BasicObservedRenderer
    {
      public:
        using ObservedType = typename RangeT::ObservedType;
        using HoldToken = CommonHoldToken<RangeT>;

        template <typename Generator = GeneratorT>
        BasicObservedRenderer(ObservedAddMutableReference_t<ObservedType>&& observed, Generator&& elementRenderer)
            : valueRange_{std::forward<ObservedAddMutableReference_t<ObservedType>>(observed)}
            , elementRenderer_{std::forward<GeneratorT>(elementRenderer)}
        {}
        virtual ~BasicObservedRenderer() = default;
        BasicObservedRenderer(BasicObservedRenderer const&) = delete;
        BasicObservedRenderer(BasicObservedRenderer&&) = delete;
        BasicObservedRenderer& operator=(BasicObservedRenderer const&) = delete;
        BasicObservedRenderer& operator=(BasicObservedRenderer&&) = delete;

        auto* getValueRange(HoldToken& holder)
        {
            return Nui::overloaded{
                [](::Nui::IsObserved auto& valueRange) {
                    return &valueRange;
                },
                [&holder](::Nui::IsWeakObserved auto& valueRange) {
                    if (holder.locked = valueRange.lock(); holder.locked)
                        return holder.locked.get();
                    return nullptr;
                },
            }(valueRange_);
        }

        bool fullRangeUpdate(auto& parent)
        {
            auto valueRangeHolder = HoldToken{};
            auto* valueRange = getValueRange(valueRangeHolder);
            if (valueRange == nullptr)
                return false;

            if (valueRange->rangeContext().isFullRangeUpdate())
            {
                parent->clearChildren();
                long long counter = 0;
                for (auto& element : valueRange->value())
                    elementRenderer_(counter++, element)(*parent, Renderer{.type = RendererType::Append});
                return true;
            }
            return false;
        }

        virtual bool updateChildren() = 0;

      protected:
        GeneratorT elementRenderer_;
        std::weak_ptr<Nui::Dom::Element> weakMaterialized_;

      private:
        ObservedAddMutableReference_t<ObservedType> valueRange_;
    };

    template <typename RangeT, typename GeneratorT>
    class RangeRenderer<RangeT, GeneratorT, true>
        : public BasicObservedRenderer<RangeT, GeneratorT>
        , public std::enable_shared_from_this<RangeRenderer<RangeT, GeneratorT, true>>
    {
      public:
        using BasicObservedRenderer<RangeT, GeneratorT>::BasicObservedRenderer;
        using BasicObservedRenderer<RangeT, GeneratorT>::weakMaterialized_;
        using BasicObservedRenderer<RangeT, GeneratorT>::elementRenderer_;
        using BasicObservedRenderer<RangeT, GeneratorT>::fullRangeUpdate;
        using BasicObservedRenderer<RangeT, GeneratorT>::getValueRange;

        void insertions(auto& parent)
        {
            auto valueRangeHolder = CommonHoldToken<RangeT>{};
            auto* valueRange = getValueRange(valueRangeHolder);
            if (valueRange == nullptr)
                return;

            if (const auto insertInterval = valueRange->rangeContext().insertInterval(); insertInterval)
            {
                for (auto i = insertInterval->low(); i <= insertInterval->high(); ++i)
                {
                    elementRenderer_(i, valueRange->value()[static_cast<std::size_t>(i)])(
                        *parent, Renderer{.type = RendererType::Insert, .metadata = static_cast<std::size_t>(i)});
                }
            }
        }

        void modifications(auto& parent)
        {
            auto valueRangeHolder = CommonHoldToken<RangeT>{};
            auto* valueRange = getValueRange(valueRangeHolder);
            if (valueRange == nullptr)
                return;

            for (auto const& range : valueRange->rangeContext())
            {
                switch (range.type())
                {
                    case RangeStateType::Keep:
                    {
                        continue;
                    }
                    case RangeStateType::Modify:
                    {
                        for (auto i = range.low(), high = range.high(); i <= high; ++i)
                        {
                            elementRenderer_(i, valueRange->value()[static_cast<std::size_t>(i)])(
                                *(*parent)[static_cast<std::size_t>(i)], Renderer{.type = RendererType::Replace});
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        bool updateChildren() override
        {
            auto parent = weakMaterialized_.lock();
            if (!parent)
                return InvalidateRange;

            Nui::ScopeExit onExit{[this] {
                auto valueRangeHolder = ::Nui::Detail::HoldToken<
                    std::decay_t<ObservedAddMutableReference_t<typename RangeT::ObservedType>>>{};
                auto* valueRange = getValueRange(valueRangeHolder);
                if (valueRange)
                    valueRange->rangeContext().reset();
            }};

            // Regenerate all elements if necessary:
            if (fullRangeUpdate(parent))
                return KeepRange;

            insertions(parent);
            modifications(parent);

            return KeepRange;
        }

        void operator()(auto& materialized)
        {
            weakMaterialized_ = materialized;

            auto valueRangeHolder = CommonHoldToken<RangeT>{};
            auto* valueRange = getValueRange(valueRangeHolder);
            if (valueRange)
            {
                valueRange->attachEvent(Nui::globalEventContext.registerEvent(Event{
                    [self = this->shared_from_this()](int) -> bool {
                        return self->updateChildren();
                    },
                    [this /* fine because other function holds this */]() {
                        return !weakMaterialized_.expired();
                    },
                }));
                updateChildren();
            }
        }
    };

    template <typename RangeT, typename GeneratorT>
    class RangeRenderer<RangeT, GeneratorT, false>
        : public BasicObservedRenderer<RangeT, GeneratorT>
        , public std::enable_shared_from_this<RangeRenderer<RangeT, GeneratorT, false>>
    {
      public:
        using BasicObservedRenderer<RangeT, GeneratorT>::BasicObservedRenderer;
        using BasicObservedRenderer<RangeT, GeneratorT>::weakMaterialized_;
        using BasicObservedRenderer<RangeT, GeneratorT>::elementRenderer_;
        using BasicObservedRenderer<RangeT, GeneratorT>::fullRangeUpdate;
        using BasicObservedRenderer<RangeT, GeneratorT>::getValueRange;

        bool updateChildren() override
        {
            auto parent = weakMaterialized_.lock();
            if (!parent)
                return InvalidateRange;

            fullRangeUpdate(parent);
            return KeepRange;
        }

        void operator()(auto& materialized)
        {
            weakMaterialized_ = materialized;
            auto valueRangeHolder = CommonHoldToken<RangeT>{};
            auto* valueRange = getValueRange(valueRangeHolder);
            if (valueRange)
            {
                valueRange->attachEvent(Nui::globalEventContext.registerEvent(Event{
                    [self = this->shared_from_this()](int) -> bool {
                        return self->updateChildren();
                    },
                    [this /* fine because other function holds this */]() {
                        return !weakMaterialized_.expired();
                    },
                }));
                updateChildren();
            }
        }
    };

    template <typename RangeLike, typename GeneratorT, typename... ObservedT>
    class UnoptimizedRangeRenderer
        : public std::enable_shared_from_this<UnoptimizedRangeRenderer<RangeLike, GeneratorT, ObservedT...>>
    {
      public:
        template <typename Generator = GeneratorT>
        UnoptimizedRangeRenderer(
            UnoptimizedRange<RangeLike, ObservedT...>&& unoptimizedRange,
            Generator&& elementRenderer)
            : unoptimizedRange_{std::move(unoptimizedRange)}
            , elementRenderer_{std::forward<GeneratorT>(elementRenderer)}
        {}

        bool updateChildren()
        {
            auto materialized = weakMaterialized_.lock();
            if (!materialized)
                return InvalidateRange;

            materialized->clearChildren();

            long long counter = 0;
            for (auto&& element : unoptimizedRange_)
            {
                elementRenderer_(counter++, ContainerWrapUtility::unwrapReferenceWrapper(element))(
                    *materialized, Renderer{.type = RendererType::Append});
            }

            return KeepRange;
        }

        void operator()(auto& materialized)
        {
            weakMaterialized_ = materialized;
            unoptimizedRange_.underlying().attachEvent(Nui::globalEventContext.registerEvent(Event{
                [self = this->shared_from_this()](int) -> bool {
                    return self->updateChildren();
                },
                [this]() {
                    return !weakMaterialized_.expired();
                },
            }));
            updateChildren();
        }

      protected:
        UnoptimizedRange<RangeLike, ObservedT...> unoptimizedRange_;
        GeneratorT elementRenderer_;
        std::weak_ptr<Nui::Dom::Element> weakMaterialized_;
    };
}