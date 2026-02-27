# Third-Party Licenses

S.A.K. Utility incorporates the following third-party software. Each
component is used under the terms of its respective license, reproduced
below.

---

## Qt 6.5.3

**License:** LGPL v3 (GNU Lesser General Public License, version 3)
**Website:** https://www.qt.io/
**Modules used:** Core, Widgets, Concurrent, Network

The Qt Toolkit is Copyright (C) 2023 The Qt Company Ltd and other
contributors.

Qt is used under the terms of the GNU Lesser General Public License
version 3 as published by the Free Software Foundation. A copy of the
LGPL v3 is available at https://www.gnu.org/licenses/lgpl-3.0.html.

You may obtain the complete corresponding Qt source code from
https://code.qt.io/ or https://download.qt.io/archive/qt/6.5/6.5.3/.

---

## aria2

**License:** GNU General Public License v2.0 (GPLv2)
**Version:** 1.37.0
**Website:** https://aria2.github.io/
**Source:** https://github.com/aria2/aria2
**Used for:** Multi-connection download manager for UUP file retrieval
**Bundled at:** `tools/uup/aria2c.exe`

aria2 is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your
option) any later version.

As required by GPLv2, the complete source code for aria2 is available at:
https://github.com/aria2/aria2/releases

---

## wimlib

**License:** GNU Lesser General Public License v3.0 (LGPL v3)
**Website:** https://wimlib.net/
**Source:** https://github.com/ebiggers/wimlib
**Used for:** WIM image manipulation during UUP-to-ISO conversion
**Bundled at:** `tools/uup/converter/bin/wimlib-imagex.exe`,
`tools/uup/converter/bin/libwim-15.dll`

```
wimlib (software) Copyright 2012-2026 Eric Biggers
```

wimlib is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version.

A copy of the LGPL v3 is available at
https://www.gnu.org/licenses/lgpl-3.0.html.

Source code is available at: https://wimlib.net/downloads/index.html

---

## 7-Zip

**License:** GNU Lesser General Public License v2.1 (LGPL v2.1) + unRAR restriction
**Website:** https://www.7-zip.org/
**Source:** https://7-zip.org/download.html
**Used for:** Archive extraction during UUP conversion
**Bundled at:** `tools/uup/converter/bin/7z.exe`, `tools/uup/converter/bin/7z.dll`

```
Copyright (C) 1999-2024 Igor Pavlov
```

7-Zip is free software distributed under the GNU LGPL. The unRAR code
is under a mixed license: GNU LGPL + unRAR restrictions. You can use
7-Zip on any computer. You don't need to register or pay for 7-Zip.

A copy of the LGPL v2.1 is available at
https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.

Source code is available at: https://7-zip.org/download.html

---

## UUP Converter (uup-converter-wimlib)

**Author:** [abbodi1406](https://github.com/abbodi1406)
**Version:** v120z
**Website:** https://uupdump.net/
**Used for:** Converting UUP update files into bootable Windows ISOs
**Bundled at:** `tools/uup/converter/`

This package bundles the convert-UUP.cmd script and supporting tools.
The converter ReadMe credits the following contributors:

- [whatever127](https://github.com/uup-dump) — UUP dump
- [Eric Biggers](https://wimlib.net) — wimlib (licensed separately above)
- [Igor Pavlov](https://www.7-zip.org) — 7-Zip (licensed separately above)
- [Melinda Bellemore](https://forums.mydigitallife.net/members/superbubble.250156/) — SxSExpand
- [BetaWorld / Secant1006](https://github.com/Secant1006/PSFExtractor) — PSFExtractor
- [Jeff Kluge](https://github.com/jeffkl/ManagedDism) — Managed DismApi Wrapper (MIT)
- [erwan.l](http://reboot.pro/files/file/313-offlinereg/) — offlinereg
- cdob — create AIO efisys.bin

### Microsoft Tools

The converter package bundles the following Microsoft utilities:
`cabarc.exe`, `cdimage.exe`, `imagex.exe`, `bcdedit.exe`

These are the intellectual property of Microsoft Corporation and are
included as part of the UUP converter package. They originate from the
Windows Assessment and Deployment Kit (Windows ADK).

### ManagedDism (Microsoft.Dism.dll)

**License:** MIT
**Source:** https://github.com/jeffkl/ManagedDism

```
Copyright (c) Jeff Kluge

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

---

## zlib

**License:** zlib License (permissive)
**Website:** https://www.zlib.net/
**Used for:** gzip compression/decompression

```
Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

Jean-loup Gailly        Mark Adler
jloup@gzip.org          madler@alumni.caltech.edu
```

---

## bzip2

**License:** BSD-style (permissive)
**Website:** https://sourceware.org/bzip2/
**Used for:** bzip2 compression/decompression

```
Copyright (C) 1996-2010 Julian Seward <jseward@bzip.org>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must
   not claim that you wrote the original software. If you use this
   software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote
   products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

---

## XZ Utils (liblzma)

**License:** 0BSD / Public Domain
**Website:** https://tukaani.org/xz/
**Used for:** xz/LZMA compression/decompression

```
Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

---

## Chocolatey

**License:** Apache License 2.0
**Website:** https://chocolatey.org/
**Used for:** Embedded portable package manager for application migration

```
Copyright 2011-2024 Chocolatey Software, Inc.
Copyright 2011-2024 RealDimensions Software, LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

---

## CMake

**License:** BSD 3-Clause
**Website:** https://cmake.org/
**Used for:** Build system (build-time only, not distributed)

---

## smartmontools

**License:** GNU General Public License v2.0 (GPLv2)
**Version:** 7.4
**Website:** https://www.smartmontools.org/
**Source:** https://github.com/smartmontools/smartmontools
**Used for:** S.M.A.R.T. disk health analysis in the Diagnostic & Benchmarking panel

smartctl.exe is bundled as a standalone executable in `tools/smartmontools/`.
The full GPLv2 license text is included alongside the binary as `COPYING`.

As required by GPLv2, the complete source code for smartmontools is available at:
https://github.com/smartmontools/smartmontools/releases

---

## vcpkg

**License:** MIT
**Website:** https://vcpkg.io/
**Used for:** C++ dependency management (build-time only, not distributed)
