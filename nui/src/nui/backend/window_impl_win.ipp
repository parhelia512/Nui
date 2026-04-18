// #####################################################################################################################
namespace Nui
{
    namespace Impl::Win
    {
        // Minimal IStream implementation that pulls bytes from a std::function reader. Only Read / basic
        // IUnknown bookkeeping / Stat are implemented — WebView2 doesn't seek response streams.
        class ReaderBackedStream : public IStream
        {
          public:
            explicit ReaderBackedStream(std::function<std::size_t(char*, std::size_t)> reader)
                : reader_{std::move(reader)}
                , refCount_{1}
            {}

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
            {
                if (ppv == nullptr)
                    return E_POINTER;
                if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ISequentialStream) ||
                    IsEqualIID(riid, IID_IStream))
                {
                    *ppv = static_cast<IStream*>(this);
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override
            {
                return static_cast<ULONG>(++refCount_);
            }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const auto c = --refCount_;
                if (c == 0)
                    delete this;
                return static_cast<ULONG>(c);
            }

            HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override
            {
                if (pv == nullptr)
                    return STG_E_INVALIDPOINTER;
                if (cb == 0)
                {
                    if (pcbRead != nullptr)
                        *pcbRead = 0;
                    return S_OK;
                }
                std::size_t n = 0;
                if (reader_)
                {
                    try
                    {
                        n = reader_(static_cast<char*>(pv), static_cast<std::size_t>(cb));
                    }
                    catch (...)
                    {
                        if (pcbRead != nullptr)
                            *pcbRead = 0;
                        return E_FAIL;
                    }
                }
                if (pcbRead != nullptr)
                    *pcbRead = static_cast<ULONG>(n);
                // Per IStream contract: S_FALSE signals end-of-stream (zero bytes available). A short read
                // (n > 0 && n < cb) is permitted by the bodyReader API and must NOT signal EOF — return S_OK.
                return (n == 0) ? S_FALSE : S_OK;
            }
            HRESULT STDMETHODCALLTYPE Write(void const*, ULONG, ULONG*) override
            {
                return STG_E_ACCESSDENIED;
            }

            HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE Commit(DWORD) override
            {
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE Revert() override
            {
                return E_NOTIMPL;
            }
            HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
            {
                return STG_E_INVALIDFUNCTION;
            }
            HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
            {
                return STG_E_INVALIDFUNCTION;
            }
            HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD /*grfStatFlag*/) override
            {
                if (pstatstg == nullptr)
                    return STG_E_INVALIDPOINTER;
                ZeroMemory(pstatstg, sizeof(STATSTG));
                pstatstg->type = STGTY_STREAM;
                pstatstg->cbSize.QuadPart = 0; // unknown
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE Clone(IStream**) override
            {
                return E_NOTIMPL;
            }

          private:
            virtual ~ReaderBackedStream() = default;

            std::function<std::size_t(char*, std::size_t)> reader_;
            std::atomic<LONG> refCount_;
        };
    } // namespace Impl::Win
    // #####################################################################################################################
    struct Window::WindowsImplementation : public Window::Implementation
    {
        DWORD windowThreadId;
        std::vector<std::function<void()>> toProcessOnWindowThread;
        EventRegistrationToken schemeHandlerToken;
        std::optional<EventRegistrationToken> setHtmlWorkaroundToken;

        WindowsImplementation()
            : Implementation{}
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

        auto webViewResult = static_cast<webview::browser_engine&>(*view).webview();
        if (!webViewResult.has_value())
            throw std::runtime_error("Could not get native webview for registering custom scheme handler!");
        auto* webView = static_cast<ICoreWebView2*>(webViewResult.value());

        for (auto const& customScheme : options.customSchemes)
        {
            const std::wstring filter = utf8ToUtf16<std::wstring, std::string>(customScheme.scheme + ":*");

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
                    // Exceptions must not cross the COM ABI back into WebView2.
                    try
                    {
                        return onSchemeRequest(schemes, view, args);
                    }
                    catch (...)
                    {
                        return E_FAIL;
                    }
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
            return utf16ToUtf8<std::wstring, std::string>(uriW);
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
        auto webViewResult = static_cast<webview::browser_engine&>(*view).webview();
        if (!webViewResult.has_value())
        {
            result = E_FAIL;
            return {};
        }
        auto* webView = static_cast<ICoreWebView2*>(webViewResult.value());

        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
        Microsoft::WRL::ComPtr<ICoreWebView2_2> wv22;
        result = webView->QueryInterface(IID_PPV_ARGS(&wv22));

        Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
        wv22->get_Environment(&environment);

        if (result != S_OK)
            return {};

        // Resolve body source in priority order: bodyFile > bodyReader > body.
        // For bodyFile, auto-populate Content-Length from file size when not already provided.
        auto headers = responseData.headers;
        int statusCode = responseData.statusCode;
        std::string phraseUtf8 = responseData.reasonPhrase;
        Microsoft::WRL::ComPtr<IStream> stream;
        if (responseData.bodyFile)
        {
            const auto& path = *responseData.bodyFile;
            const auto pathW = path.wstring();
            const HRESULT fileHr = SHCreateStreamOnFileEx(
                pathW.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
            if (SUCCEEDED(fileHr))
            {
                if (headers.find("Content-Length") == headers.end())
                {
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(path, ec);
                    if (!ec)
                        headers.emplace("Content-Length", std::to_string(size));
                }
            }
            else
            {
                // Surface the failure as 500 instead of silently emitting an empty 200 — silent
                // success-with-empty-body is a cache-poisoning footgun.
                static constexpr char errorBody[] = "Internal Server Error: bodyFile could not be opened.";
                statusCode = 500;
                if (phraseUtf8.empty())
                    phraseUtf8 = "Internal Server Error";
                stream.Attach(
                    SHCreateMemStream(reinterpret_cast<const BYTE*>(errorBody), sizeof(errorBody) - 1));
                headers.erase("Content-Length");
                headers.emplace("Content-Length", std::to_string(sizeof(errorBody) - 1));
                headers.erase("Content-Type");
                headers.emplace("Content-Type", "text/plain; charset=utf-8");
            }
        }
        else if (responseData.bodyReader)
        {
            stream.Attach(new Impl::Win::ReaderBackedStream{responseData.bodyReader});
        }
        else
        {
            stream.Attach(SHCreateMemStream(
                reinterpret_cast<const BYTE*>(responseData.body.data()),
                static_cast<UINT>(responseData.body.size())));
        }

        std::wstring responseHeaders;
        for (auto const& [key, value] : headers)
            responseHeaders += utf8ToUtf16<std::wstring, std::string>(key) + L": " +
                utf8ToUtf16<std::wstring, std::string>(value) + L"\r\n";
        if (!responseHeaders.empty())
        {
            responseHeaders.pop_back();
            responseHeaders.pop_back();
        }

        const auto phrase = utf8ToUtf16<std::wstring, std::string>(phraseUtf8);
        result = environment->CreateWebResourceResponse(
            stream.Get(), statusCode, phrase.c_str(), responseHeaders.c_str(), &response);

        return response;
    }
    //---------------------------------------------------------------------------------------------------------------------
    CustomSchemeRequest Window::WindowsImplementation::makeCustomSchemeRequest(
        CustomScheme const& customScheme,
        std::string const& uri,
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext,
        ICoreWebView2WebResourceRequest* webViewRequest)
    {
        const bool streaming = customScheme.streamingContent;

        // Hold a ref on the request so body-reading lambdas stay safe even if a user copies them out
        // of the synchronous onRequest scope (the API allows it; we don't want UAF when they do).
        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> requestRef{webViewRequest};

        using GetContentVariant =
            std::variant<std::function<std::string const&()>, std::function<std::string()>>;

        auto getContentStreaming = GetContentVariant{std::function<std::string()>{}};
        auto getContentBuffered = GetContentVariant{std::function<std::string const&()>{
            [requestRef, contentMemo = std::string{}]() mutable -> std::string const& {
                if (!contentMemo.empty())
                    return contentMemo;

                Microsoft::WRL::ComPtr<IStream> stream;
                requestRef->get_Content(&stream);

                if (!stream)
                    return contentMemo;

                constexpr ULONG bufferSize = 16 * 1024;
                std::array<char, bufferSize> buffer;
                ULONG bytesRead = 0;
                do
                {
                    stream->Read(buffer.data(), bufferSize, &bytesRead);
                    contentMemo.append(buffer.data(), bytesRead);
                } while (bytesRead == bufferSize);
                return contentMemo;
            }}};

        auto readContentStreaming = std::function<std::size_t(char*, std::size_t)>{
            [requestRef, stream = Microsoft::WRL::ComPtr<IStream>{}, initialized = false](
                char* buffer, std::size_t bufferSize) mutable -> std::size_t {
                if (!initialized)
                {
                    requestRef->get_Content(&stream);
                    initialized = true;
                }
                if (!stream)
                    return 0;
                ULONG bytesRead = 0;
                const HRESULT hr = stream->Read(buffer, static_cast<ULONG>(bufferSize), &bytesRead);
                if (FAILED(hr))
                    return 0;
                return static_cast<std::size_t>(bytesRead);
            }};

        return CustomSchemeRequest{
            .scheme = customScheme.scheme,
            .getContent = streaming ? std::move(getContentStreaming) : std::move(getContentBuffered),
            .readContent = streaming ? std::move(readContentStreaming)
                                     : std::function<std::size_t(char*, std::size_t)>{},
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

                        headersMap.emplace(
                            utf16ToUtf8<std::wstring, std::string>(nameW),
                            utf16ToUtf8<std::wstring, std::string>(valueW));

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
                    return utf16ToUtf8<std::wstring, std::string>(methodW);
                }(),
            .resourceContext = static_cast<NuiCoreWebView2WebResourceContext>(resourceContext),
        };
    }
    // #####################################################################################################################
}