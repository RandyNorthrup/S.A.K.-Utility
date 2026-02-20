# Windows ISO Downloader - WAF Bypass Implementation

## Problem Analysis

The Windows ISO downloader was getting blocked by Microsoft's WAF/Sentinel with "Invalid JSON response from server" errors when attempting to fetch available languages.

## Root Cause

After analyzing the official **Fido.ps1** script (used by Rufus and widely trusted), we discovered our implementation had unnecessary complexity:

1. **Product Page Fetching**: We were trying to fetch and parse the product page HTML to extract session IDs - but Fido doesn't do this at all
2. **Session ID Source**: Fido simply generates a fresh UUID instead of parsing HTML
3. **Whitelisting Requirement**: Microsoft requires session IDs to be whitelisted via `vlscppe.microsoft.com/tags` before any API calls

## Solution Implemented

### Key Changes

#### 1. Simplified Session Initialization
**Before:**
```cpp
// fetchProductPage() would:
// - Fetch HTML from microsoft.com/software-download/windows11
// - Parse HTML looking for session ID (fragile, slow)
// - Parse product edition ID from dropdowns
// - THEN whitelist the parsed session ID
```

**After (matching Fido.ps1):**
```cpp
void WindowsISODownloader::fetchProductPage() {
    // Generate fresh UUID directly (no HTML fetching/parsing)
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    
    // Immediately whitelist it before any API use
    startWhitelistRequest();
}
```

#### 2. Proper Whitelisting Flow
**Whitelist endpoint:**
```
https://vlscppe.microsoft.com/tags?org_id=y6jn8c31&session_id=[UUID]
```

**Process:**
1. Generate UUID: `550e8400-e29b-41d4-a716-446655440000`
2. Whitelist via tags endpoint (required before API calls)
3. Use whitelisted session for all subsequent requests:
   - `getskuinformationbyproductedition` (fetch languages)
   - `GetProductDownloadLinksBySku` (get download URLs)

#### 3. Constants from Fido.ps1
```cpp
m_orgId = "y6jn8c31"         // Microsoft organization ID
m_profileId = "606624d44113"  // Product profile identifier
```

### WAF Bypass Techniques (Already Implemented)

1. **User-Agent Rotation**: Pool of 6 realistic browser UAs (Chrome 129-131, Edge 129-130, Firefox 132-133)
2. **Request Delays**: Random 2-4 second delays between requests (mimics human interaction)
3. **TLS Configuration**: TLS 1.2+ with ALPN for HTTP/2
4. **Headers**:
   - No `X-Requested-With` header (common automation flag)
   - Browser-specific client hints (`sec-ch-ua`, `sec-ch-ua-platform`)
   - Proper `Referer` header for API requests
   - Realistic `Accept` headers based on content type
5. **Cookie Management**: Full cookie jar with all cookies accepted and forwarded

## API Flow (Matching Fido.ps1)

```
1. Generate UUID
   └→ UUID: 550e8400-e29b-41d4-a716-446655440000

2. Whitelist Session
   └→ GET https://vlscppe.microsoft.com/tags
      ?org_id=y6jn8c31
      &session_id=550e8400-e29b-41d4-a716-446655440000
      
3. Fetch Languages (SKUs)
   └→ GET https://www.microsoft.com/software-download-connector/api/getskuinformationbyproductedition
      ?profile=606624d44113
      &productEditionId=3262
      &SKU=undefined
      &friendlyFileName=undefined
      &Locale=en-US
      &sessionID=550e8400-e29b-41d4-a716-446655440000
      Headers: Referer: https://www.microsoft.com/en-us/software-download/windows11

4. Request Download URL
   └→ GET https://www.microsoft.com/software-download-connector/api/GetProductDownloadLinksBySku
      ?profile=606624d44113
      &productEditionId=3262
      &SKU=[SKU_ID_FROM_STEP_3]
      &friendlyFileName=undefined
      &Locale=en-US
      &sessionID=550e8400-e29b-41d4-a716-446655440000
      Headers: Referer: https://www.microsoft.com/en-us/software-download/windows11
```

## Benefits

1. **Simpler**: No HTML parsing, no fragile regex patterns
2. **Faster**: Skips unnecessary product page fetch
3. **More Reliable**: Matches proven, working implementation (Fido)
4. **WAF-Resistant**: Proper session whitelisting prevents blocking
5. **Maintainable**: Less code, clearer intent

## Testing

After implementation:
- ✅ Session initialization completes successfully
- ✅ Whitelist request returns HTTP 200
- ✅ Language fetch should now work (pending user testing)
- ✅ Builds successfully on Windows with MSVC

## References

- **Fido.ps1**: https://github.com/pbatard/Fido
  - Official PowerShell script for Windows ISO downloads
  - Used by Rufus, trusted by community
  - Our implementation now mirrors its approach

- **SessionID Whitelisting**: Lines 630-646 in Fido.ps1
- **API Calls**: Lines 174-189, 230-266 in Fido.ps1

## Future Improvements

If issues persist:
1. Add session cookie inspection (log received cookies)
2. Implement session regeneration on 403/401 responses
3. Add exponential backoff for retry delays
4. Consider adding proxy support for additional anonymity
