# Third-Party Notices

This document lists third-party software incorporated into QUADmapper.

---

## Snappy (decode-only, vendored)

**Location:** `src/source/hap/snappy/SnappyDecoder.{hpp,cpp}`

**Description:** A clean-room decode-only implementation of the Snappy block
decompression algorithm, used to decompress HAP video frame payloads.

**Algorithm reference:** Google Snappy block format specification  
https://github.com/google/snappy/blob/main/format_description.txt

**Original Snappy library copyright:**

> Copyright 2011, Google Inc.  
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> * Redistributions of source code must retain the above copyright notice,
>   this list of conditions and the following disclaimer.
> * Redistributions in binary form must reproduce the above copyright notice,
>   this list of conditions and the following disclaimer in the documentation
>   and/or other materials provided with the distribution.
> * Neither the name of Google Inc. nor the names of its contributors may be
>   used to endorse or promote products derived from this software without
>   specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

**Note:** The QUADmapper implementation (`SnappyDecoder.cpp`) is a clean-room
reimplementation using the published format specification; it does not include
any code from the original Snappy library. The BSD-3-Clause terms of the
Snappy algorithm reference are reproduced here for attribution.

---

## HAP Video Codec (format specification reference)

**Location:** `src/source/hap/` (HapFrameParser, HapFormat)

**Specification:** HAP Video Draft specification  
https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md

> Copyright (c) 2012-2013, Tom Butterworth and Vidvox LLC.  
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> * Redistributions of source code must retain the above copyright notice,
>   this list of conditions and the following disclaimer.
> * Redistributions in binary form must reproduce the above copyright notice,
>   this list of conditions and the following disclaimer in the documentation
>   and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

---

## Bundled fonts (SIL Open Font License 1.1)

The redesigned UI (ADR-045) bundles two OFL-1.1 licensed fonts under `resources/fonts/`,
redistributed unmodified:

- **Hanken Grotesk** — © The Hanken Grotesk Project Authors
  (https://github.com/displaay/Hanken-Grotesk). UI typography.
- **JetBrains Mono** — © 2020 The JetBrains Mono Project Authors
  (https://github.com/JetBrains/JetBrainsMono). Numeric values and file paths.

Both are licensed under the SIL Open Font License, Version 1.1
(https://scripts.sil.org/OFL). Full license text ships with each font's upstream
distribution; the OFL permits bundling and redistribution with the application.
