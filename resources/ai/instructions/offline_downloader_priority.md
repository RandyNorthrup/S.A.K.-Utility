# Offline Downloader Priority

For offline installers and offline deployment requests, use SAK built-in tooling
first.

Order:

1. Search SAK package/offline tooling for candidate package IDs.
2. Use direct offline installer download when user asks for installer files.
3. Use offline bundle build when user asks for deployment media or multiple apps.
4. Use official vendor web download only after the SAK offline path fails or is
   not applicable.

Always report output path, file size, SHA-256 when available, and source.
