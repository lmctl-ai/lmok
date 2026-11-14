# Third-Party Notices

## tree-sitter

This repository builds against, and statically links, **tree-sitter** — an
incremental parsing library.

- Upstream: https://tree-sitter.github.io / https://github.com/tree-sitter/tree-sitter
- Version: 0.27.1 (v0.27.1), pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2018 Max Brunsfeld and contributors
- Full license text: the upstream `LICENSE` file, vendored at
  `third_party/tree-sitter/LICENSE` after running `scripts/sync-deps.sh`

## tree-sitter-json

The JSON grammar compiled into the executable (`third_party/tree-sitter-json`).

- Upstream: https://github.com/tree-sitter/tree-sitter-json
- Revision: 254c42a6476413b776221e03982ac8ae159eeb72, pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2014 Max Brunsfeld and contributors
- Full license text: the upstream `LICENSE` file, vendored at
  `third_party/tree-sitter-json/LICENSE` after running `scripts/sync-deps.sh`

## tree-sitter-c

The C grammar compiled into the executable (`third_party/tree-sitter-c`).

- Upstream: https://github.com/tree-sitter/tree-sitter-c
- Revision: b780e47fc780ddc8da13afa35a3f4ed5c157823d (master), pinned in
  `deps.lock`
- License: **MIT License**
- Copyright (c) 2014 Max Brunsfeld and contributors
- Full license text: the upstream `LICENSE` file, vendored at
  `third_party/tree-sitter-c/LICENSE` after running `scripts/sync-deps.sh`

## tree-sitter-go

The Go grammar compiled into the executable (`third_party/tree-sitter-go`).

- Upstream: https://github.com/tree-sitter/tree-sitter-go
- Version: 0.25.0, pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2014 Max Brunsfeld
- Full license text: the upstream `LICENSE` file, vendored at
  `third_party/tree-sitter-go/LICENSE` after running `scripts/sync-deps.sh`

## tree-sitter-javascript

The JavaScript grammar compiled into the executable (`third_party/tree-sitter-javascript`).

- Upstream: https://github.com/tree-sitter/tree-sitter-javascript
- Version: 0.25.0, pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2014 Max Brunsfeld
- Full license text: the upstream `LICENSE` file, vendored at
  `third_party/tree-sitter-javascript/LICENSE` after running `scripts/sync-deps.sh`

## tree-sitter-typescript

The TypeScript and TSX grammars compiled into the executable (`third_party/tree-sitter-typescript/typescript` and `third_party/tree-sitter-typescript/tsx`).

- Upstream: https://github.com/tree-sitter/tree-sitter-typescript
- Version: 0.23.2, pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2017 Max Brunsfeld
- Full license text: the upstream `LICENSE` file, vendored at
  `third_party/tree-sitter-typescript/LICENSE` after running `scripts/sync-deps.sh`

## lmcode-cpp SDK

`cpp/dslvm.hpp`, the `cpp/stdlib/*.hpp` headers, and
`cpp/std/lmcode-std.schema.json` are vendored from lmcode-cpp (MIT License,
Lattice Systems LLC), pinned by `sdk.lock` and synced by
`scripts/sync-sdk.sh`.

## SQLite

The SQLite 3.54.1 amalgamation is compiled into the executable with FTS5.

- Upstream: https://www.sqlite.org/
- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
- License: public domain (https://www.sqlite.org/copyright.html)
- Archive SHA-256 pinned in `deps.lock`; source at `third_party/sqlite/`.

## tree-sitter-java

The Java grammar compiled into the executable.

- Upstream: https://github.com/tree-sitter/tree-sitter-java
- Version: v0.23.5, archive SHA-256 pinned in deps.lock
- License: MIT License
- Copyright (c) 2017 Ayman Nadeem
- Full license text: third_party/tree-sitter-java/LICENSE after scripts/sync-deps.sh

## tree-sitter-python

The Python grammar compiled into the executable (`third_party/tree-sitter-python`).

- Upstream: https://github.com/tree-sitter/tree-sitter-python
- Version: 0.23.6, archive SHA-256 pinned in `deps.lock`
- License: MIT License
- Copyright (c) 2016 Max Brunsfeld
- Full license text is embedded below.

## tree-sitter-bash

The Bash grammar compiled into the executable (`third_party/tree-sitter-bash`).

- Upstream: https://github.com/tree-sitter/tree-sitter-bash
- Version: 0.25.1, archive SHA-256 pinned in `deps.lock`
- License: MIT License
- Copyright (c) 2017 Max Brunsfeld
- Full license text is embedded below.

## Embedded license texts

The following license texts are embedded in `lmok --notices`, so release artifacts remain self-contained.

### lmcode-cpp SDK

MIT License. Copyright (c) Lattice Systems LLC. Permission is granted, free
of charge, to use, copy, modify, merge, publish, distribute, sublicense, and
sell copies of the Software, subject to inclusion of this copyright and
permission notice. The Software is provided "AS IS", without warranty.

### SQLite

SQLite is in the public domain and may be used for any purpose without
restriction. The public-domain dedication is documented at
https://www.sqlite.org/copyright.html.

### tree-sitter

```text
The MIT License (MIT)

Copyright (c) 2018 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-json

```text
The MIT License (MIT)

Copyright (c) 2014 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-c

```text
The MIT License (MIT)

Copyright (c) 2014 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-python

```text
The MIT License (MIT)

Copyright (c) 2016 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-bash

```text
The MIT License (MIT)

Copyright (c) 2017 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-go

```text
The MIT License (MIT)

Copyright (c) 2014 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-javascript

```text
The MIT License (MIT)

Copyright (c) 2014 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-typescript

```text
The MIT License (MIT)

Copyright (c) 2017 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### tree-sitter-java

```text
MIT License

Copyright (c) 2017 Ayman Nadeem

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## tree-sitter-lmscript

The lmscript parser and external scanner compiled into the executable.

- Upstream: https://github.com/lmctl-ai/tree-sitter-lmscript
- Revision: ff301218f388f30fcf40cd63d235070c162aa283, pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2026 Lattice Systems LLC
- Full license text: `third_party/tree-sitter-lmscript/LICENSE` after dependency sync

## tree-sitter-lmcode

The lmcode parser and external scanner compiled into the executable.

- Upstream: https://github.com/lmctl-ai/tree-sitter-lmcode
- Revision: c9d5021cc98982f558937298e4add9a4f79c574a, pinned in `deps.lock`
- License: **MIT License**
- Copyright (c) 2026 Lattice Systems LLC
- Full license text: `third_party/tree-sitter-lmcode/LICENSE` after dependency sync

### tree-sitter-lmscript and tree-sitter-lmcode — full MIT license

```text
MIT License

Copyright (c) 2026 Lattice Systems LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## tree-sitter-cpp

The C++ parser and external scanner compiled into the executable.

- Upstream: https://github.com/tree-sitter/tree-sitter-cpp
- Version: 0.23.4 (`v0.23.4`, pinned in `deps.lock`)
- License: **MIT License**
- Copyright (c) 2014 Max Brunsfeld
- Full license text: `third_party/tree-sitter-cpp/LICENSE` after dependency sync

### tree-sitter-cpp — full MIT license

```text
The MIT License (MIT)

Copyright (c) 2014 Max Brunsfeld

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
