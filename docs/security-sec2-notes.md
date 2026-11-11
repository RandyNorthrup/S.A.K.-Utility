# Security fix round sec2: listeners, hostile remote data, signing and gate pins (2026-11-11)

For `docs/RELEASE_READINESS.md`, security section; main merges this text there and deletes this
file. Items 5 and 6 of the security fix round, plus the two items "also inside the digest". Every
finding was re-read in the code at 77a92478 before it was fixed; all were real. Each fix has a test
that went red with the fix taken out and green with it restored (drill results in the sec2
report).

## 5. Network and IPC listeners

**iPerf3 server.** It listened on every address the PC had, kept a peer that connected and went
quiet for as long as it liked, and started whether or not its inbound firewall rule was added
(unelevated, `netsh` refuses, and Windows then asked the user whether to let `iperf3.exe` through,
public networks included). Now (`bandwidth_tester.cpp`): `-B <address>` binds it to the one
adapter address chosen in the panel, `--idle-timeout 30` drops a silent peer and goes back to
listening, and a server other PCs can reach starts only after its rule is in place. The rule also
names the bound address (`localip=`). Without the rule the server is not started and the panel
says why: "iPerf3 server not started: Windows Firewall did not add its inbound rule (...). Adding
it needs S.A.K. running as administrator." A loopback server needs no rule (Windows Firewall does
not filter loopback). Tests: `test_bandwidth_tester` (the argv; Windows' own listener table shows
127.0.0.1 alone; with the rule refused, no server and the message).

**LAN Transfer server.** It listened on `QHostAddress::Any`. It now listens on the chosen adapter
address and refuses a wildcard. Test: `test_network_diagnostic_controller` (the listener table
shows 127.0.0.1 alone; Any refused).

**"Listen on" (new UI, small).** Neither server had any choice of interface, so each server row in
the Network Diagnostics panel gains a "Listen on:" list of the connected adapters' IPv4 addresses
(Bandwidth tab, iPerf3 group, before Start Server; LAN Transfer tab, server group, before Listen
Port). Each list starts on the adapter holding the default route and remembers the address that
server was last started on (`data/config/ui_state.ini`, `NetworkDiagnostics/*ServerListenAddress`).
Tests: `test_network_diagnostic_controller` (choices, preference), `test_network_diagnostic_panel`
(default, per-server memory). Screens to retake: Network Diagnostics > Bandwidth, and Network
Diagnostics > LAN Transfer.

**Browser bridge pipe.** The handshake read the first frame against the 64 MiB native-messaging
limit and checked the token, and only then the peer: any same-user process that opened the pipe
held its single instance for the 30 s deadline and could make the server allocate up to 64 MiB.
Now the peer (our own binary, launched by Chrome) is checked the moment it connects, before a byte
is read, and the hello's length prefix is refused above 4 KiB before anything is allocated.
Replies keep the full limit (they may carry screenshots). Tests: `test_browser_bridge_pipe` (a
60 MiB prefix is dropped at once with no allocation of 1 MiB or more; Windows PowerShell as a
foreign peer is dropped at once and the real relay is admitted right after).

## 6. Treating the remote side as hostile

**Installer downloads.** The internalization engine (installers and `.nupkg`s) and the direct
downloader held each download whole in RAM, up to 4 GiB, to hash it before writing. They now share
`PackageInternalizationEngine::downloadVerifiedFile`: the body streams to a file in the process's
temp folder (`QDir::tempPath()`, the per-run folder the app points TEMP at), then is copied into
place through a `QSaveFile` while it is hashed, so the bytes that land are the bytes that were
checked, and nothing is committed on a mismatch. An installer with no usable checksum is now
refused before it is downloaded rather than after. `runNetworkTransfer` gained a body sink, and
refuses to hold any response that may exceed 512 MiB in memory: a caller that could get one must
stream it. Tests: `test_package_internalization_engine` (64 MiB over loopback with no Qt allocation
of 1 MiB or more, staged in the temp folder and never beside the target; a mismatch leaves
nothing; the cap stops a streamed body), `test_bandwidth_tester` (the in-memory ceiling).

**HFS+ leaf chains.** None of the four leaf walks (extents overflow, catalog listing, catalog
consistency scan, attributes scan) remembered the nodes it had visited, so a leaf linking back
into the chain re-read the same records up to the 200,000-node cap: a listing repeated entries, a
scan counted them again, and the extents load appended them to every fork's extent list. Each walk
now stops at a node reached twice: "HFS+ <tree> leaf chain loops back to node N". Test:
`test_partition_manager_core` (`hfsFileSystemReader_refusesLeafChainLoops`, all four walks).

**zlib.** The APFS decmpfs decoder (inline and ZLIB_RSRC chunks) used `qUncompress`, which grows
its buffer to whatever the stream holds, and compared the size afterwards. The same shape was in
the HFS+ decmpfs chunk decoder and the PST 4K block reader. All three now use one
`inflateZlibExactly` that inflates into the declared size plus one byte and stops there. Tests:
`test_bounded_inflate`, `test_partition_manager_core` (APFS and HFS+ bombs), `test_pst_parser`
(a 32 MiB bomb declaring 4 KiB: no allocation of 1 MiB or more).

**APFS extended attributes.** A B-tree node's table entries were bounds-checked one by one but
never against each other, so many entries could point at one record: a 1 MiB leaf of 100,000
entries naming one 64 KiB attribute value had the scan hold 100,000 copies (gigabytes). And two
records for one attribute both survived, with the decmpfs one (the file's content) decided by scan
order. Now a node whose entries overlap is structurally invalid, and a second record for one
(object, name) is refused. Test: `test_partition_manager_core`
(`apfsFileSystemReader_refusesAliasedOrRepeatedXattrs`, both on writer-made images patched and
re-checksummed).

## Also inside the digest

**sign-exe.ps1.** It accepted any valid signature on the Artifact Signing dlib, which signtool runs
with signing rights, and any valid signature on the exe it had just signed. Now every DLL in the
dlib's folder must be signed by Microsoft (the client's own DLLs are "Microsoft Corporation", the
VC runtime it carries "Microsoft Windows"), and the signed exe must be signed by
`CN=Randy Northrup, O=Randy Northrup` (the publisher `verify_authenticode_signatures.ps1` requires
of every packaged binary) and timestamped. Test: `test_sign_exe_signers` (release-tooling): the
checks on real signed files, and `sign-exe.ps1` itself run in a sandbox with a stand-in `az`,
stopping at a Chocolatey-signed dlib.

**requirements-gates.txt.** Every package pip installs from it, the three tools and the twelve they
pull in, is pinned to one version and to the SHA-256 of every file PyPI publishes for it, so pip
runs in hash-checking mode. Proved on testpc2 in a fresh venv: installs and runs clang-format
23.1.3, lizard 1.24.3 and pre-commit 4.7.0; a wrong hash and an unlisted dependency are both
refused. The CI action that reads the clang-format pin parsed `clang-format==(.+)$`, which the new
line continuation would have broken (" \" captured with the version); it now reads the version
only.

## Proof (testpc2, W:\sak at the sec2 tree)

- Build: Visual Studio 2022 / Qt 6.12.0, Release, BUILD EXIT 0, 293 executables.
- Targeted tests (console session, standard-user token): 15/15 green.
- Drills: 26 guards, each broken alone, red, restored byte for byte, rebuilt, green: 23 C++
  (zlib x4, HFS+ walks x4, APFS x2, downloads x4, pipe x2, servers and panel x7) and 3 PowerShell
  (sign-exe). One first attempt (iPerf3 `-B` removed) did not compile, so its green proved
  nothing; it was rewritten to compile and went red.
- T2 labels (product, release-tooling, tooling, cert), `-j 6`: 323 of 324 passed;
  `test_partition_manager_rig_pc_target` failed for testpc2's own state, not this change: the
  final certification left `C:\SAKCert\disposable-machine.json` there, so the rig script passes
  its disposable-machine check and stops at its administrator check, where the test expects the
  "runs only in a VirtualBox" refusal. sec2 changes none of the scripts involved.
- clang-tidy naming gate, both passes (LLVM 23.1.3): non-core 155/155 TUs clean; core 171/171
  healthy, 15 ratcheted TUs clean.
- ci.yml static gates, whole tree: all pass (hygiene, registration, qrc, PowerShell syntax,
  clang-format, lizard, ASCII, secret regex, licenses, blocking, GUI tokens / magic numbers /
  stylesheet literals, magic numbers, dialog buttons, logged message boxes, error-message
  uniqueness, build-system lint).

## Residual risks

- Before secfix's TEMP change lands, `QDir::tempPath()` is the user's %TEMP%, so until then a
  partial download is staged there. After the merge it is the app's per-run folder.
- The offline deployment build still keeps its work folders in the chosen output folder
  (`<output>\_work`, `<output>\_sak_temp_XXXXXX`), which the owner's "work files only under data\"
  rule would move; unchanged here.
- iPerf3 and the LAN server bind IPv4 adapter addresses only (the list offers no IPv6).
- The LAN transfer server still adds no firewall rule of its own; bound to one address, Windows
  asks about it as before.
- iperf3's own handling of a hostile client is upstream's; the idle limit and the bind narrow who
  can reach it and for how long.
- The exe timestamp check moved into `authenticode_signer.ps1` unchanged; no validly signed file
  without a timestamp was at hand to drill it.

## For the merge onto lane2

Four tree deletes in the files this round changed still call `QDir::removeRecursively`. sec2 did
not touch them; at the merge each switches to secfix's `sak::no_follow::removeTree` (a one-line
change at each), with secfix's junction-canary test for each in the follow-up commit:

- `offline_deployment_worker.cpp` `OfflineDeploymentWorker::removeOwnedWorkDir`
  (`QDir(work_dir).removeRecursively()`; line 304 at 77a92478, 283 on sec2).
- `package_internalization_engine.cpp` `PackageInternalizationEngine::beginInternalization`
  (`QDir(paths.extract_dir).removeRecursively()`; line 520 at 77a92478, 540 on sec2).
- `package_internalization_engine.cpp` `PackageInternalizationEngine::cleanNugetArtifacts`, twice
  (`rels_dir.removeRecursively()` and `pkg_dir.removeRecursively()`; lines 1213 and 1223 at
  77a92478, 1282 and 1292 on sec2).

The streamed download's own staging file is a `QTemporaryFile` in `QDir::tempPath()`, removed by
its destructor (one file, no tree), so it needs no change at the merge.

## CHANGELOG lines (user-visible), for the next release section

- **The network test servers listen on one adapter** - The iPerf3 server and the LAN Transfer server
  listened on every network the PC was on. Each now has a "Listen on" list of the connected
  adapters' addresses: it starts on the adapter holding the default route and remembers the one
  you last used for that server. The iPerf3 server drops a peer that connects and goes quiet, and
  starts only when its Windows Firewall rule is in place: run as a standard user, it no longer
  starts half-working while Windows asks whether to let iperf3 through, and says it needs S.A.K.
  running as administrator instead.
- **Offline installer downloads no longer fill memory** - Bundling a package read each installer
  whole into memory, up to 4 GiB, before checking it. Downloads now go to disk as they arrive and
  are checked as they are copied into the bundle, so only a file that matches its checksum lands
  there; an installer whose package declares no usable checksum is refused before it is
  downloaded.
- **Crafted disk images and mail files are refused sooner** - An HFS+ image whose B-tree leaves
  loop, an APFS image with overlapping B-tree entries or two copies of one extended attribute, and
  a compressed APFS or HFS+ file or Outlook block that inflates past its declared size are now
  refused before they can repeat records or use large amounts of memory.
