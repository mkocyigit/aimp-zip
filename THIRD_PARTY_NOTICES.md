# Third-party notices

The MIT license in `LICENSE` covers only this project's own code (`src/`, build scripts).
The components below remain under their own terms.

## miniz

Used for reading zip archives. Fetched at build time and compiled into `aimp_zip.dll`.
<https://github.com/richgel999/miniz> (version 3.0.2)

```
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## AIMP SDK (v6.00 build 3000)

The `sdk/` folder contains the AIMP plugin SDK, © 2006-2026 Artem Izmaylov,
<https://www.aimp.ru>. It is included unmodified for building. The SDK ships
without a separate license file; all rights belong to its author.

## BASS

Tracker modules are decoded with `bass.dll`, © Un4seen Developments
(<https://www.un4seen.com>). It is **not** distributed with this plugin; the
plugin uses the copy already loaded by AIMP.

## ZenLib, zlib

The `ZenLib/` (MediaArea.net, zlib-like license) and `zlib/` (zlib license)
folders keep their original license files. They are not used in the build.
