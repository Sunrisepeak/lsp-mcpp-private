# lspgen input

`metaModel-3.18.json` is the Language Server Protocol meta model, version 3.18.0,
copied unmodified from
[microsoft/vscode-languageserver-node](https://github.com/microsoft/vscode-languageserver-node/blob/main/protocol/metaModel.json)
under the MIT license in `LICENSE-vscode-languageserver-node.txt`.

`mcppls-lspgen generate` turns it into `src/lsp/protocol.cppm` and
`src/lsp/protocol.cpp`. CI regenerates both files and fails when they differ
from the committed ones.

```bash
mcpp run mcppls-lspgen -- generate --meta-model tools/lspgen/metaModel-3.18.json --out src/lsp
```
