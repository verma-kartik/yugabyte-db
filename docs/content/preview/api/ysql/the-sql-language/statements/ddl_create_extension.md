---
title: CREATE EXTENSION statement [YSQL]
headerTitle: CREATE EXTENSION
linkTitle: CREATE EXTENSION
summary: Load an extension into a database
description: Use the CREATE EXTENSION statement to load an extension into a database.
menu:
  preview:
    identifier: ddl_create_extension
    parent: statements
aliases:
  - /preview/api/ysql/commands/ddl_create_extension/
type: docs
---

## Synopsis

Use the `CREATE EXTENSION` statement to load an extension into a database.

## Syntax

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link active" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <i class="fa-solid fa-file-lines" aria-hidden="true"></i>
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <i class="fa-solid fa-diagram-project" aria-hidden="true"></i>
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="grammar" class="tab-pane fade show active" role="tabpanel" aria-labelledby="grammar-tab">
  {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_extension.grammar.md" %}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
  {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_extension.diagram.md" %}}
  </div>
</div>

## Semantics

- `SCHEMA`, `VERSION`, and `CASCADE` may be reordered.

## Examples

```plpgsql
CREATE SCHEMA myschema;
CREATE EXTENSION pgcrypto WITH SCHEMA myschema VERSION '1.3';
```

```output
CREATE EXTENSION
```

```plpgsql
CREATE EXTENSION IF NOT EXISTS earthdistance CASCADE;
```

```output
NOTICE:  installing required extension "cube"
CREATE EXTENSION
```

## See also

- [PostgreSQL Extensions](../../../../../explore/ysql-language-features/pg-extensions/)
- [DROP EXTENSION](../ddl_drop_extension)
