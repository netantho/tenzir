---
sidebar_position: 0
---

# Pipelines

The **Tenzir Query Language (TQL)**[^1] centers around one core principle:
dataflow pipelines.

[^1]: We pronounce it *teaquel*.

A pipeline is chain of [operators](../operators.md) that represents a flow of
data. Operators can produce, transform, or consume data. Think of it as UNIX
pipes or Powershell commands where output from one command is input to the next:

![Pipeline Chaining](pipeline-chaining.excalidraw.svg)

Tenzir distinguishes three types of operators:

1. **Source**: generates new data
2. **Transformation**: modifies data
3. **Sink**: consumes data

A pipeline consists of one source, one sink, and zero or more transformations.
The diagram below illustrates the operator chaining:

![Pipeline Structure](pipeline-structure.excalidraw.svg)

If a pipeline would not have a source and sink, it would "leak" data. We call
pipelines that have both a source and sink a **closed pipeline**. Tenzir can
only execute closed pipelines. A pipeline that solely consists of a chain
transformations is an **open pipeline**.

## Syntax

Tenzir comes with its own language to define pipelines, dubbed Tenzir Query
Language (TQL). Even though it is a transformation language, we are calling it
"query language" to allude to the outcome of getting the data in the desired
form. The language is geared towards working with richly typed, structured event
data across multiple schemas.

There exist numerous dataflow languages out there, and we drew inspiration from
others to achieve:

- the *familiarity* of [splunk](https://splunk.com)
- the *power* of [Kusto](https://github.com/microsoft/Kusto-Query-Language)
- the *expressiveness* of [dplyr](https://dplyr.tidyverse.org/)
- the *flexibility* of [jq](https://stedolan.github.io/jq/)
- the *ambition* of [Zed](https://zed.brimdata.io/)

:::tip Why yet another language?
You may sigh and ask "why are you creating yet another language?" We hear you.
Please allow us to elaborate. First, our long-term strategy is to support as
many language *frontends* as possible, similar to Databricks building a
[SIEM-to-Spark transpiler](https://github.com/databrickslabs/transpiler). Our
committment to Apache Arrow and efforts like [substrait](https://substrait.io/)
and [ibis](https://ibis-project.org/) further show that there is a desire for
convergence. Unfortunately we cannot wait until these fledling projects are
production-grade; the needed [data engineering has a long
tail](/blog/parquet-and-feather-data-engineering-woes).

Second, our goal is to build an open system on top of Apache Arrow, allowing
anyone to hook into a standardized data stream to deploy analytics at scale.
None of the existing dataflow languages used by security people have this
property. Neither did we encounter the capability to write operators that work
across multiple schemas in a rich type system. We do not want users to think of
tables, but rather domain types. Finally, users should be able to express both
streaming and batch workloads in a unified language.

In summary, speed of iteration, the current data ecosystem state, the
commitment to an open data plane, the focus on types as opposed tables, and the
unified execution of streaming and batch workloads drove us to devising a new
language. We hope that you can appreciate and follow this reasoning as you
unpack the language.
:::

More generally, we put a lot of emphasis on the following guidelines when
designing the language:

1. Use natural language keywords where possible, e.g., verbs to convey actions.
2. Lean on operator names that are familiar to Unix and Powershell users
3. Avoid gratuitous syntax elements like brackets, braces, quotes, or
   punctuations.
4. Exploit symmetries for an intuitive learning experience, e.g., `from` and
   `to` have their duals `read` and `write`.

How does the syntax of a concrete Tenzir pipeline look? Let's take the following
example:

![Pipeline Example](pipeline-example.excalidraw.svg)

Here is how you write this pipeline in the Tenzir language:

```cpp
/* 1. Start pipeline to data at a Tenzir node */
export
/* 2. Filter out a subset of events (predicate gets pushed down) */
| where #schema == "zeek.weird" && note == "SSL::Invalid_Server_Cert"
/* 3. Aggregate them by destination IP */
| summarize count(num) by id.resp_h
/* 4. Sort by frequency */
| sort
/* 5. Take the top-20 items */
| head 20
/* 6. Write the output as JSON to standard output */
| write json to stdout
```

## Expressions

Tenzir [expressions](expressions) are search expressions to describe the desired
working set, usually at the beginning of an interactive data exploration. An
expression consists of predicates chained together by *connectives*, such as a
conjunction (logical AND), a disjunction (logical OR), and a negation (logical
NOT). The expressiveness is equivalent to [boolean
algebra](https://en.wikipedia.org/wiki/Boolean_algebra) and its laws.

Expression occur predominantly as argument to the
[where](../operators/transformations/where.md) operator to filter the dataflow.
Other expression elements, such as [extractors](expressions#extractors), also
occur in other operators.

Historically, the Tenzir language only supported providing expressions. But the
strong demand for reshaping and analytic workloads made the language evolve to
include dataflow semantics.

## Connectors and Formats

Tenzir has two low-level abstractions to integrate with the rest of the world:

- [Connector](connectors.md): performs low-level I/O to exchange data with a
  resource. A connector provides a *loader* to acquire raw bytes, and/or a
  *saver* to send raw bytes to an external resource.
- [Format](formats.md): translates bytes into structured events and vice versa.
  A format provides a *parser* that generates events or a *printer* that
  translates events into raw bytes.

The following diagram illustrates the dataflow between connectors, formats, and
the remaining operators:

![Connectors & Formats](connector-format.excalidraw.svg)

A connector is typically I/O-bound whereas a format is CPU-bound.

## Logical and Phyiscal Operators

Tenzir has two types of operators:

1. **Logical Operators**: user-facing operators that express the intent in the
   domain.
2. **Physical Operators**: data-facing implementation detail.

Let's consider an example of a logical pipeline with the source consisting of
`from` and `read`, and the sink `write` and `to`:

![Logical Plan](operator-logical.excalidraw.svg)

A given `from`-`read` and `write`-`to` combination often directly maps to its
physical counterpart, the `load`-`parse` and `print`-`save` operators that do
the actual work with the help of a connector and format:

![Physical Plan](operator-physical.excalidraw.svg)

However, this rigid mapping is not required. The interfaces for sources and
sinks only demand that the operator generates or consumes structured events.
Some sources do the loading and parsing in a single step, e.g., when going
through a third-party library that exposes the structured data directly. We'd
still like to write `from X` at the logical level in this case, but the physical
then won't go through `load` and `parse`.

This decoupling is what makes the Tenzir language declarative: the user only
needs to specify the intent of how the data flows, but it's up to the
implementation to select the optimal building blocks for the most efficient
realization. For example, filter predicates may be "pushed down" to reduce the
amount of data that flows through a pipeline. Such optimizations are common
practice in declarative query languages.
