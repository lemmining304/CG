# CG (C Git)

> Um clone de Git em C, construido em camadas pequenas e verificaveis.

![language](https://img.shields.io/badge/language-C-00599C)
![stage](https://img.shields.io/badge/stage-MVP-orange)
![build](https://img.shields.io/badge/build-make-informational)

`CG` implementa os fundamentos do Git com foco em simplicidade de codigo,
aprendizado de internals e evolucao incremental.

## Estado Atual

- [x] CLI basica (`cg --help`, `cg --version`)
- [x] `cg init [diretorio]`
- [x] Inicializacao de repositorio `.git` valido com:
- [x] `objects/`
- [x] `refs/heads/`
- [x] `refs/tags/`
- [x] `HEAD` apontando para `refs/heads/main`
- [x] `config` e `description`

## Quickstart

```bash
make
./cg --help
./cg init
./cg init meu-projeto
```

## Exemplo Real

```bash
$ ./cg init demo
Initialized empty CG repository in /.../demo/.git

$ git -C demo rev-parse --is-inside-work-tree
true
```

## Arquitetura Inicial

- `src/main.c`: parser de comandos e dispatch de subcomandos
- helpers locais para:
- criacao segura de diretorios
- escrita de arquivos de metadata (`HEAD`, `config`, `description`)
- composicao de caminhos com validacao de tamanho

## Estrutura do Projeto

```text
cg/
|-- Makefile
|-- README.md
|-- .gitignore
`-- src/
    `-- main.c
```

## Roadmap

### Fase 1: Inspecao do Working Tree

- [ ] `cg status` (tracked, modified, untracked)
- [ ] leitura basica de `.gitignore`

### Fase 2: Area de Staging

- [ ] `cg add <path>`
- [ ] escrita e leitura do `index`

### Fase 3: Historico

- [ ] `cg commit -m "<msg>"`
- [ ] criacao de objetos `blob`, `tree`, `commit`
- [ ] atualizacao de `refs/heads/main`
- [ ] `cg log`

### Fase 4: Navegacao

- [ ] `cg branch`
- [ ] `cg checkout <branch|commit>`

## Nao Objetivos (Agora)

- rede (`clone`, `fetch`, `push`)
- merge/rebase
- compatibilidade total com todos os cantos do Git

## Contribuicao

Contribuicoes sao bem-vindas para:

- novos subcomandos pequenos e bem testados
- melhoria de mensagens de erro
- testes de integracao por comando
