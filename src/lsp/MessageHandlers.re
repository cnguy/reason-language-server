
open Result;
open State;
open Infix;

let extend = (obj, items) => Json.obj(obj) |?>> current => Json.Object(current @ items);

let log = Log.log;

let maybeHash = (h, k) => if (Hashtbl.mem(h, k)) { Some(Hashtbl.find(h, k)) } else { None };
type handler = Handler(string, Json.t => result('a, string), (state, 'a) => result((state, Json.t), string)) : handler;

let handlers: list((string, (state, Json.t) => result((state, Json.t), string))) = [
  ("textDocument/definition", (state, params) => {
    open InfixResult;
    params |> RJson.get("textDocument") |?> RJson.get("uri") |?> RJson.string
    |?> uri => RJson.get("position", params) |?> Protocol.rgetPosition
    |?> position => State.getPackage(uri, state) |?> package => {
      switch (State.getDefinitionData(uri, state, ~package)) {
      | None => {
        Error("Parse error, can't find definition")
      }
      | Some(data) =>
        Log.log("Ok have definition data for " ++ uri);
        switch (State.definitionForPos(uri, position, data, state, ~package)) {
        | Some((_, None, _, _))
        | Some((_, _, _, None))
        | None => Ok((state, Json.Null))
        | Some((_, Some(loc), docs, Some(uri))) => Ok((state, Json.Object([
          ("uri", Json.String(uri)),
          ("range", Protocol.rangeOfLoc(loc)),
        ])))
        }
      }
    }
  }),

  /** TODO implement */
  ("textDocument/signatureHelp", (state, params) => {
    Ok((state, Json.Null))
  }),

  ("textDocument/completion", (state, params) => {
    open InfixResult;
    (Protocol.rPositionParams(params) |?> ((uri, pos)) => (maybeHash(state.documentText, uri) |> orError("No document text found"))
    |?> ((text, version, isClean)) => State.getPackage(uri, state) |?> package => (PartialParser.positionToOffset(text, pos) |> orError("invalid offset")) |?>> offset => {
        open Rpc.J;
        let completions = switch (PartialParser.findCompletable(text, offset)) {
        | Nothing => {
          Log.log("Nothing completable found :/");
          []
        }
        | Labeled(string) => {
          Log.log("don't yet support completion for argument labels, but I hope to soon!");
          []
        }
        | Lident(string) => {
          log("Completing for string " ++ string);
          let parts = Str.split(Str.regexp_string("."), string);
          let parts = string.[String.length(string) - 1] == '.' ? parts @ [""] : parts;
          let currentModuleName = String.capitalize(Filename.chop_extension(Filename.basename(uri)));
          let opens = PartialParser.findOpens(text, offset);
          /* */
          let localData = State.getLastDefinitions(uri, state);
          let useMarkdown = !state.settings.clientNeedsPlainText;
          Completions.get(~currentPath=Infix.(Utils.parseUri(uri) |? "current file"), currentModuleName, opens, parts, state, localData, pos, ~package) |> List.map(({Completions.kind, path, label, detail, documentation}) => o([
            ("label", s(label)),
            ("kind", i(Completions.kindToInt(kind))),
            ("detail", Infix.(detail |?>> s |? null)),
            ("documentation", Infix.((documentation |?>> d => d ++ "\n\n" ++ fold(path, "", path => (useMarkdown ? "*" : "") ++ (
              Utils.startsWith(path, state.rootPath ++ "/") ? Utils.sliceToEnd(path, String.length(state.rootPath ++ "/")) : path
              ) ++ (useMarkdown ? "*" : ""))) |?>> Protocol.contentKind(useMarkdown) |? null)),
            ("data", switch kind {
              | RootModule(cmt, src) => o([("cmt", s(cmt)), ("name", s(label)), ...(fold(src, [], src => [("src", s(src))]))])
              | _ => null
              })
          ]))
        }
        };
        (state, l(completions))
      }
    );
  }),

  ("completionItem/resolve", (state, params) => {
    switch (params |> Json.get("documentation") |?> Json.string) {
    | Some(_) => Ok((state, params))
    | None =>
      let result = (params |> Json.get("data")
      |?> data => Json.get("cmt", data) |?> Json.string
      |?> cmt => Json.get("src", data) |?> Json.string
      |?> src => Json.get("name", data) |?> Json.string
      |?>> name => {
        let (detail, docs) = Completions.getModuleResults(name, state, cmt, Some(src));

        open Rpc.J;
        extend(params, [
          ("detail", detail |?>> s |? null),
          ("documentation", docs |?>> Protocol.contentKind(!state.settings.clientNeedsPlainText) |? null),
        ]) |? params
      }) |? params;
      Ok((state, result))
    }
  }),

  ("textDocument/documentHighlight", (state, params) => {
    open InfixResult;
    Protocol.rPositionParams(params) |?> ((uri, pos)) => State.getPackage(uri, state) |?>> package => {
      open Infix;
      let highlights = (State.getDefinitionData(uri, state, ~package) |?> data => Definition.highlights(pos, data)) |? [];
      open Rpc.J;
      (state, l(highlights |> List.map(((t, loc)) => o([
        ("range", Protocol.rangeOfLoc(loc)),
        ("kind", i(switch t {
        | `Read => 2
        | `Write => 3
        }))
        ]))))
    };
  }),

  ("textDocument/references", (state, params) => {
    open InfixResult;
    Protocol.rPositionParams(params) |?> ((uri, pos)) => State.getPackage(uri, state) |?>> package => {
      open Infix;
      (State.getDefinitionData(uri, state, ~package)
      |?> data =>
      switch (Definition.openReferencesAtPos(data, pos)) {
        | Some(references) => Some((state, Json.Array(references |> List.map(((_, _, loc)) => Protocol.locationOfLoc(~fname=uri, loc)))))
        | None =>
          State.referencesForPos(uri, pos, data, state, ~package)
          |?>> allReferences => {
            open Rpc.J;
            (
              state,
              l(
                allReferences
                |> List.map(
                    ((fname, references)) =>
                      (fname == uri ? List.filter(((_, loc)) => !Protocol.locationContains(loc, pos), references) : references) |>
                      List.map(((_, loc)) => Protocol.locationOfLoc(~fname, loc))
                  )
                |> List.concat
              )
            );
          }
      }) |? (state, Json.Null)
    };
  }),

  ("textDocument/rename", (state, params) => {
    open InfixResult;
    Protocol.rPositionParams(params) |?> ((uri, pos)) => State.getPackage(uri, state) |?> package => RJson.get("newName", params) |?> RJson.string
    |?> newName => {
      open Infix;
      (State.getDefinitionData(uri, state, ~package)
      |?> data => State.referencesForPos(uri, pos, data, state, ~package)
      |?>> allReferences => {
        open Rpc.J;
        Ok((state, o([
          ("changes", o(allReferences |> List.map(((uri, positions)) =>
            (uri, l(positions |> List.map(((_, loc)) => o([
              ("range", Protocol.rangeOfLoc(loc)),
              ("newText", s(newName)),
            ]))))
          )))
        ])))
      }) |? Ok((state, Json.Null))
    };
  }),

  ("textDocument/codeLens", (state, params) => {
    open InfixResult;
    let%try uri = params |> RJson.get("textDocument") |?> RJson.get("uri") |?> RJson.string;
    /* let%try package = State.getPackage(uri, state); */
    switch (State.getPackage(uri, state)) {
      | Result.Error(message) => {
        let items = [("Unable to load compilation data: " ++ message, {
          Location.loc_start: {Lexing.pos_fname: "", pos_lnum: 1, pos_bol: 0, pos_cnum: 0},
          Location.loc_end: {Lexing.pos_fname: "", pos_lnum: 1, pos_bol: 0, pos_cnum: 0},
          loc_ghost: false,
        })];
        open Rpc.J;
        Ok((state, l(items |> List.map(((text, loc)) => o([
          ("range", Protocol.rangeOfLoc(loc)),
          ("command", o([
            ("title", s(text)),
            ("command", s(""))
          ]))
        ])))))
      }
      | Ok(package) =>
      open Infix;
    let items = {
      let%opt moduleData = {
        let%opt (_, moduleData) = State.getCompilationResult(uri, state, ~package) |> AsYouType.getResult;
        Some(moduleData)
      } |?# lazy(State.getLastDefinitions(uri, state));

      let showToplevelTypes = state.settings.perValueCodelens; /* TODO config option */
      let lenses = showToplevelTypes ? moduleData.Definition.topLevel |> Utils.filterMap(({Docs.T.name, loc, kind, docstring}) => {
        switch kind {
        | Docs.T.Value(t) => PrintType.default.expr(PrintType.default, t) |> PrintType.prettyString |> s => Some((s, loc))
        | _ => None
        }
      }) : [];

      let showOpens = state.settings.opensCodelens;
      let lenses = showOpens ? lenses @ Definition.opens(moduleData) : lenses;

      let showDependencies = state.settings.dependenciesCodelens;
      let lenses = showDependencies ? [("Dependencies: " ++ String.concat(", ", Definition.dependencyList(moduleData)), {
        Location.loc_start: {Lexing.pos_fname: "", pos_lnum: 1, pos_bol: 0, pos_cnum: 0},
        Location.loc_end: {Lexing.pos_fname: "", pos_lnum: 1, pos_bol: 0, pos_cnum: 0},
        loc_ghost: false,
      }), ...lenses] : lenses;

      Some(lenses)
    };
    switch items {
    | None => Error("Could not get compilation data")
    | Some(items) =>
      open Rpc.J;
      Ok((state, l(items |> List.map(((text, loc)) => o([
        ("range", Protocol.rangeOfLoc(loc)),
        ("command", o([
          ("title", s(text)),
          ("command", s(""))
        ]))
      ])))))
    }
    }
  }),

  ("textDocument/hover", (state, params) => {
    open InfixResult;
    let%try (uri, (line, character)) = Protocol.rPositionParams(params);
    let%try package = State.getPackage(uri, state);
    open Rpc.J;
    let%try result = Hover.getHover(uri, line, character, state, ~package);
    Ok({
      switch result {
      | None => (state, Json.Null)
      | Some((text, loc)) =>
        (state, o([
          ("range", Protocol.rangeOfLoc(loc)),
          ("contents",  text |> Protocol.contentKind(!state.settings.clientNeedsPlainText))
        ]))
      }
    })
  }),

  ("textDocument/rangeFormatting", (state, params) => {
    open InfixResult;
    params |> RJson.get("textDocument") |?> RJson.get("uri") |?> RJson.string
    |?> uri => State.getPackage(uri, state)
    |?> package => RJson.get("range", params) |?> Protocol.rgetRange
    |?> ((start, end_)) => {
      let text = State.getContents(uri, state);
      open Infix;
      (PartialParser.positionToOffset(text, start)
      |?> startPos => PartialParser.positionToOffset(text, end_)
      |?>> endPos => {
        let substring = String.sub(text, startPos, endPos - startPos);
        open InfixResult;
        AsYouType.format(substring, package.refmtPath) |?>> text => {
          open Rpc.J;
          (state, l([o([
            ("range", Infix.(|!)(Json.get("range", params), "what")),
            ("newText", s(text))
          ])]))
        }
      }) |? Error("Invalid position")
    }
  }),

  ("textDocument/documentSymbol", (state, params) => {
    open InfixResult;
    params |> RJson.get("textDocument") |?> RJson.get("uri") |?> RJson.string
    |?> uri => State.getPackage(uri, state) |?> package => {
      open Infix;
      /* let items = */
      let items = State.getCompilationResult(uri, state, ~package) |> AsYouType.getResult |?>> snd
      |?# lazy(State.getLastDefinitions(uri, state))
      |?>> ((({Definition.topLevel})) => {
        let rec getItems = (path, item) => {
          /* let (name, loc, item, docs, scopeRange) = Hashtbl.find(stamps, stamp); */
          let res = switch (item.Docs.T.kind) {
            | Docs.T.Module(contents) => {
              let children = contents |> List.map(getItems(path @ [item.name])) |> List.concat;
              Some((`Module, children))
            }
            | Type(t, _extra) => Some((Protocol.typeKind(t), []))
            | Value(v) => Some((Protocol.variableKind(v), []))
            | _ => None
          };
          res |?>> ((((typ, children))) => [(item.name, typ, item.loc, path), ...children]) |? []
        };
        topLevel |> List.map(getItems([])) |> List.concat;
      });
      (items |?>> items => {
        open Rpc.J;
        Ok((state, l(items |> List.map(((name, typ, loc, path)) => o([
          ("name", s(name)),
          ("kind", i(Protocol.symbolKind(typ))),
          ("location", Protocol.locationOfLoc(loc)),
          ("containerName", s(String.concat(".", path)))
        ])))))

      }) |? Ok((state, Json.Null))
    }
  }),

  ("textDocument/formatting", (state, params) => {
    open InfixResult;
    params |> RJson.get("textDocument") |?> RJson.get("uri") |?> RJson.string
    |?> uri => State.getPackage(uri, state)
    |?> package => {
      let text = State.getContents(uri, state);
      AsYouType.format(text, package.refmtPath) |?>> newText => {
        open Rpc.J;
        (state, text == newText ? Json.Null : l([o([
          ("range", Protocol.rangeOfInts(
            0, 0,
            List.length(Str.split(Str.regexp_string("\n"), text)) + 1, 0
          )),
          ("newText", s(newText))
        ])]))
      }
    }
  })
];

