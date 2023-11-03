// #####################################################################################################################
namespace
{
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>
    webView2EnvironmentOptionsFromOptions(WindowOptions const& options)
    {
        auto environmentOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();

        if (options.browserArguments)
        {
            const auto wideArgs = widenString(*options.browserArguments);
            environmentOptions->put_AdditionalBrowserArguments(wideArgs.c_str());
        }

        if (options.language)
        {
            const auto wideLanguage = widenString(*options.language);
            environmentOptions->put_Language(wideLanguage.c_str());
        }

        Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions4> options4;
        if (environmentOptions.As(&options4) == S_OK)
        {
            std::vector<Microsoft::WRL::ComPtr<CoreWebView2CustomSchemeRegistration>> customSchemeRegistrations;
            std::vector<std::vector<std::wstring>> allowedOrigins;
            std::vector<std::vector<std::wstring::value_type const*>> allowedOriginsRaw;
            std::vector<std::wstring> wideSchemes;

            allowedOrigins.reserve(options.customSchemes.size());
            allowedOriginsRaw.reserve(options.customSchemes.size());
            wideSchemes.reserve(options.customSchemes.size());

            for (const auto& customScheme : options.customSchemes)
            {
                wideSchemes.push_back(widenString(customScheme.scheme));
                customSchemeRegistrations.push_back(
                    Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(wideSchemes.back().c_str()));
                auto& customSchemeRegistration = customSchemeRegistrations.back();

                allowedOrigins.push_back({});
                allowedOrigins.back().reserve(customScheme.allowedOrigins.size());
                for (const auto& allowedOrigin : customScheme.allowedOrigins)
                    allowedOrigins.back().push_back(widenString(allowedOrigin));

                allowedOriginsRaw.push_back({});
                allowedOriginsRaw.back().reserve(allowedOrigins.back().size());
                for (const auto& allowedOrigin : allowedOrigins.back())
                    allowedOriginsRaw.back().push_back(allowedOrigin.c_str());

                customSchemeRegistration->SetAllowedOrigins(
                    static_cast<UINT>(allowedOriginsRaw.back().size()), allowedOriginsRaw.back().data());
                customSchemeRegistration->put_TreatAsSecure(customScheme.treatAsSecure);
                customSchemeRegistration->put_HasAuthorityComponent(customScheme.hasAuthorityComponent);
            }
            std::vector<ICoreWebView2CustomSchemeRegistration*> customSchemeRegistrationsRaw;
            customSchemeRegistrationsRaw.reserve(customSchemeRegistrations.size());
            for (const auto& customSchemeRegistration : customSchemeRegistrations)
                customSchemeRegistrationsRaw.push_back(customSchemeRegistration.Get());

            const auto result = options4->SetCustomSchemeRegistrations(
                static_cast<UINT>(customSchemeRegistrationsRaw.size()), customSchemeRegistrationsRaw.data());
            if (FAILED(result))
                throw std::runtime_error("Could not set custom scheme registrations.");
        }

        Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions5> options5;
        if (environmentOptions.As(&options5) == S_OK)
        {
            options5->put_EnableTrackingPrevention(options.enableTrackingPrevention);
        }

        return environmentOptions;
    }
}

namespace Nui
{
    // #####################################################################################################################
    struct Window::WindowsImplementation : public Window::Implementation
    {
        DWORD windowThreadId;
        std::vector<std::function<void()>> toProcessOnWindowThread;
        EventRegistrationToken schemeHandlerToken;
        std::optional<EventRegistrationToken> setHtmlWorkaroundToken;

        WindowsImplementation(WindowOptions const& options)
            : Implementation{options}
            , windowThreadId{GetCurrentThreadId()}
            , toProcessOnWindowThread{}
            , schemeHandlerToken{}
            , setHtmlWorkaroundToken{}
        {}

        void registerSchemeHandlers(WindowOptions const& options) override;
        HRESULT onSchemeRequest(
            std::vector<CustomScheme> const& schemes,
            ICoreWebView2*,
            ICoreWebView2WebResourceRequestedEventArgs* args);
        CustomSchemeRequest makeCustomSchemeRequest(
            CustomScheme const& scheme,
            std::string const& uri,
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext,
            ICoreWebView2WebResourceRequest* webViewRequest);
        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse>
        makeResponse(CustomSchemeResponse const& responseData, HRESULT& result);
    };
    //---------------------------------------------------------------------------------------------------------------------
    void Window::WindowsImplementation::registerSchemeHandlers(WindowOptions const& options)
    {
        using namespace std::string_literals;

        auto* webView = static_cast<ICoreWebView2*>(static_cast<webview::browser_engine&>(view).webview());

        for (auto const& customScheme : options.customSchemes)
        {
            const std::wstring filter = widenString(customScheme.scheme + ":*");

            auto result = webView->AddWebResourceRequestedFilter(
                filter.c_str(), static_cast<COREWEBVIEW2_WEB_RESOURCE_CONTEXT>(NuiCoreWebView2WebResourceContext::All));

            if (result != S_OK)
                throw std::runtime_error(
                    "Could not AddWebResourceRequestedFilter: "s + std::to_string(result) +
                    " for scheme: " + customScheme.scheme);
        }

        webView->add_WebResourceRequested(
            Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this, schemes = options.customSchemes](
                    ICoreWebView2* view, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    return onSchemeRequest(schemes, view, args);
                })
                .Get(),
            &schemeHandlerToken);
    }
    //---------------------------------------------------------------------------------------------------------------------
    HRESULT
    Window::WindowsImplementation::onSchemeRequest(
        std::vector<CustomScheme> const& schemes,
        ICoreWebView2*,
        ICoreWebView2WebResourceRequestedEventArgs* args)
    {
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext;
        auto result = args->get_ResourceContext(&resourceContext);
        if (result != S_OK)
            return result;

        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> webViewRequest;
        args->get_Request(&webViewRequest);

        const auto uri = [&webViewRequest]() {
            LPWSTR uri;
            webViewRequest->get_Uri(&uri);
            std::wstring uriW{uri};
            CoTaskMemFree(uri);
            return shortenString(uriW);
        }();

        const auto customScheme = [&schemes, &uri]() -> std::optional<CustomScheme> {
            // assuming short schemes list, a linear search is fine
            for (auto const& scheme : schemes)
            {
                if (uri.starts_with(scheme.scheme + ":"))
                    return scheme;
            }
            return std::nullopt;
        }();

        if (!customScheme)
            return S_OK;

        if (!customScheme->onRequest)
            return S_OK;

        CustomSchemeRequest request =
            makeCustomSchemeRequest(*customScheme, uri, resourceContext, webViewRequest.Get());
        auto response = makeResponse(customScheme->onRequest(request), result);

        if (result != S_OK)
            return result;

        result = args->put_Response(response.Get());
        return result;
    }
    //---------------------------------------------------------------------------------------------------------------------
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse>
    Window::WindowsImplementation::makeResponse(CustomSchemeResponse const& responseData, HRESULT& result)
    {
        auto* webView = static_cast<ICoreWebView2*>(static_cast<webview::browser_engine&>(view).webview());

        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
        Microsoft::WRL::ComPtr<ICoreWebView2_2> wv22;
        result = webView->QueryInterface(IID_PPV_ARGS(&wv22));

        Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
        wv22->get_Environment(&environment);

        if (result != S_OK)
            return {};

        std::wstring responseHeaders;
        for (auto const& [key, value] : responseData.headers)
            responseHeaders += widenString(key) + L": " + widenString(value) + L"\r\n";
        if (!responseHeaders.empty())
        {
            responseHeaders.pop_back();
            responseHeaders.pop_back();
        }

        Microsoft::WRL::ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(
            reinterpret_cast<const BYTE*>(responseData.body.data()), static_cast<UINT>(responseData.body.size())));

        const auto phrase = widenString(responseData.reasonPhrase);
        result = environment->CreateWebResourceResponse(
            stream.Get(), responseData.statusCode, phrase.c_str(), responseHeaders.c_str(), &response);

        return response;
    }
    //---------------------------------------------------------------------------------------------------------------------
    CustomSchemeRequest Window::WindowsImplementation::makeCustomSchemeRequest(
        CustomScheme const& customScheme,
        std::string const& uri,
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext,
        ICoreWebView2WebResourceRequest* webViewRequest)
    {
        return CustomSchemeRequest{
            .scheme = customScheme.scheme,
            .getContent = [webViewRequest, contentMemo = std::string{}]() mutable -> std::string const& {
                if (!contentMemo.empty())
                    return contentMemo;

                Microsoft::WRL::ComPtr<IStream> stream;
                webViewRequest->get_Content(&stream);

                if (!stream)
                    return contentMemo;

                // FIXME: Dont read the whole thing into memory, if possible via streaming.
                ULONG bytesRead = 0;
                do
                {
                    std::array<char, 1024> buffer;
                    stream->Read(buffer.data(), 1024, &bytesRead);
                    contentMemo.append(buffer.data(), bytesRead);
                } while (bytesRead == 1024);
                return contentMemo;
            },
            .headers =
                [webViewRequest]() {
                    ICoreWebView2HttpRequestHeaders* headers;
                    webViewRequest->get_Headers(&headers);

                    Microsoft::WRL::ComPtr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
                    headers->GetIterator(&iterator);

                    std::unordered_multimap<std::string, std::string> headersMap;
                    for (BOOL hasCurrent; SUCCEEDED(iterator->get_HasCurrentHeader(&hasCurrent)) && hasCurrent;)
                    {
                        LPWSTR name;
                        LPWSTR value;
                        iterator->GetCurrentHeader(&name, &value);
                        std::wstring nameW{name};
                        std::wstring valueW{value};
                        CoTaskMemFree(name);
                        CoTaskMemFree(value);

                        headersMap.emplace(shortenString(nameW), shortenString(valueW));

                        BOOL hasNext = FALSE;
                        if (FAILED(iterator->MoveNext(&hasNext)) || !hasNext)
                            break;
                    }
                    return headersMap;
                }(),
            .uri = uri,
            .method =
                [webViewRequest]() {
                    LPWSTR method;
                    webViewRequest->get_Method(&method);
                    std::wstring methodW{method};
                    CoTaskMemFree(method);
                    return shortenString(methodW);
                }(),
            .resourceContext = static_cast<NuiCoreWebView2WebResourceContext>(resourceContext),
        };
    }
    // #####################################################################################################################
}