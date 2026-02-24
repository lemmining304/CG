# CG

> C Git: um clone minimalista de Git escrito em C.

![Language](https://img.shields.io/badge/language-C-00599C)
![Status](https://img.shields.io/badge/status-MVP-orange)
![Build](https://img.shields.io/badge/build-make-informational)

CG e um projeto focado em aprender e implementar o nucleo do Git passo a passo,
com codigo simples, legivel e incremental.

## Features (MVP)

- [x] `cg init [diretorio]`
- [x] Cria repositorio `.git` valido com:
- [x] `objects/`
- [x] `refs/heads/`
- [x] `refs/tags/`
- [x] `HEAD`
- [x] `config`
- [x] `description`

## Quick Start

```bash
make
./cg --help
./cg init
./cg init meu-projeto
```

## Exemplo

```bash
$ ./cg init demo
Initialized empty CG repository in /.../demo/.git
```

## Estrutura

```text
cg/
|-- Makefile
|-- README.md
`-- src/
    `-- main.c
```

## Roadmap

- [ ] `cg status` (arquivos rastreados e nao rastreados)
- [ ] `cg add` (staging/index)
- [ ] `cg commit` (objetos blob/tree/commit)
- [ ] `cg log`
- [ ] `cg branch`
- [ ] `cg checkout`

## Objetivo

Construir um clone funcional de Git em C, com foco em:

- internals do Git (objetos, refs, index e commits)
- implementacao incremental e testavel
- base didatica para estudos de sistemas e ferramentas de versao
