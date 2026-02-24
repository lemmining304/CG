# CG (C Git)

CG e um clone minimo de Git escrito em C.

## Estado atual (MVP)

- Comando implementado: `cg init [diretorio]`
- Cria um repositorio Git valido em `.git` com:
- `objects/`
- `refs/heads/`
- `refs/tags/`
- `HEAD`
- `config`
- `description`

## Build

```bash
make
```

## Uso

```bash
./cg init
./cg init meu-projeto
```

## Proximos passos

1. `cg status` (arquivos rastreados e nao rastreados)
2. `cg add` (index/staging area)
3. `cg commit` (objetos blob/tree/commit)
