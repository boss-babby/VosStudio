# Live AI

A real-time voice / text session with the Gemini Live API, shown in a light pop-up at the lower right of the window.
The assistant hears the microphone, answers with its voice (or text) and works the application through the agent's
tools while it talks. This document describes how it is built and how to check it on a Windows machine.

## Using it

- **Open**: the *Live* button in the top bar (waves icon), `Ctrl+Shift+L`, or *Live AI* in the command palette.
  The pop-up connects at once and shows `Ready — type a message`. Nothing is shown when the pop-up is closed.
- **Talk**: the microphone button in the composer (or *Live AI: start talking* in the palette). A text session is
  re-opened as a voice session: a Live session has exactly one response modality, so the switch starts a **new**
  session (the server refuses to resume a text session as a voice one — that was the *response modalities (AUDIO,
  TEXT) is not supported* error of 1.9.1) and the conversation so far is handed to the model as context; the
  transcript shows *Switching to voice · the conversation continues in a new session.* The status line shows
  `Listening` with a level meter, `Hearing you…` while you talk and `Thinking…` once you stop; while the assistant
  speaks, the microphone pauses (`mic paused`) unless the *barge-in* option is on in Settings → Live AI. The stop
  button in the composer cuts the assistant's speech.
- **Turn-taking (1.9.5)**: the application decides when you have stopped talking, not the server. Its speech
  detector (`live::SpeechDetector`: level against an adaptive noise floor, 160 ms to start, 900 ms of quiet to end)
  sends `activityStart`, the audio and `activityEnd`, and the session is set up with the server's automatic
  activity detection **off**. With the server's detection (1.9.4 and earlier) the 3.x Live models regularly
  transcribed an utterance and then never answered: the turn stayed open, and the next sentence only closed it,
  unanswered — the "it writes what I said but does not respond" symptom (reported on the Live models at large,
  e.g. google-gemini/gemini-live-api-examples #38 and #40; measured there at 9–42 % of utterances, ~0 % with
  client-side markers). A watchdog covers the rest: if nothing comes back `liveWatchdogS` (6) seconds after you
  stopped (no speech, no *thinking*, no tool call), the pop-up notes it and asks the model once to answer; an empty
  turn closed with `NEED_MORE_INPUT` / `RESPONSE_REJECTED` is answered the same way. Settings → Live AI: *The app
  detects when I stop talking* (setting `liveClientVad`, script `livevad 0|1`, palette *Live AI: toggle speech
  detection in the app*); hidden tuning: `liveVadDb` (quietest level counted as speech, default −48 dBFS),
  `liveVadEndMs` (default 900; also the server's `silenceDurationMs` when its detection is used).
- **Compare, scope and Word (1.10.0)**: `show_compare` puts two periods, two imported files or two thresholds side by
  side in the main area (or colours the map as a difference map) and returns the counts; `focus_cluster` now also
  scopes the Trends and Actors pages, the papers table and the geo view to the cluster's records (linked selection;
  `cluster: 0` clears it, and `get_ui_state` reports the scope); `export_report` takes `format: pdf|docx|both` — the
  Word file has real headings, tables, embedded figures and references. 41 tools in total.
- **Word export rebuilt (1.10.1)**: the `.docx` written by `export_report` / *Save as Word…* now converts the
  assistant's Markdown faithfully (joined paragraphs, nested and numbered lists, quotes, code, pipe tables, inline
  formatting, clickable links and DOIs) and validates against the Open XML schemas, so Word opens it without repair.
- **Your records are safe from the assistant (1.9.9)**: `new_project` starts a blank project (refused over unsaved
  work unless `discard_unsaved` is true — the model is told to ask you first; Undo restores the old records and map);
  `search_openalex` needs `append: true` or `replace: true` once records are loaded; opening a project over unsaved
  work needs `replace: true`; `run_commands` applies the same rule to `oafetch` and `openproj`. Refusals name the
  alternatives. Tool results describe what really happened (`preview R12` → "open in the inspector: <title>" or "no
  such record"), `get_ui_state` says what fills the main area, and the prompt says: report only what the results
  confirm. `show_papers` opens the papers table (filter, sort) and returns the first rows; `show_chart` puts any report
  chart into the main area at canvas size (or switches to a map view) and returns what it shows, so the assistant can
  walk you through several figures while it talks. Since 1.13 the `split` and `pane` commands (`run_commands`) put
  two or four charts — or charts and the live map — side by side (`split 4 map publications_per_year top_sources
  strategic_diagram`; `pane 2 most_cited`; `split off`), and `get_ui_state` lists what each pane shows.
- **The top bar stayed usable (1.9.9)**: with the pop-up open, clicks on the search field and the top-bar buttons were
  taken as window drags, because the overlay-only frames forgot where those widgets were. Fixed (`Ui::beginFrame`
  partial frames keep the last full frame's widget map, hovered widget, tooltip and cursor).
- **Heard more clearly (1.9.8)**: the microphone is a *Communications* stream (the device's noise suppression / echo
  cancellation / gain where available; hidden setting `liveMicVoice`), quiet voices are brought up by an automatic
  gain before sending (`liveAgc`), the detector takes quieter speech (`liveVadDb` -56), keeps 600 ms of pre-roll and
  waits 1100 ms of silence (1400 ms after a short fragment) before ending a turn. While the assistant speaks with
  barge-in off, clear sustained speech still stops it and starts your turn. Playback keeps a 250 ms reserve so bursts
  from the server do not leave gaps in sentences.
- **Measured, not assumed (1.9.6)**: `tests/live_probe.py` speaks a WAV file into a real session the way the
  application does (16 kHz PCM in 40 ms chunks, the same setup message) and prints the timeline. Twenty-odd sessions
  on the two 3.8 Live models in September 2026 gave three facts that shaped this version. (1) Some spoken turns are
  transcribed and then never answered — no `interactionStatus`, no audio, no `turnComplete`, for as long as one
  waits — while the same question as text is answered every time. All of those sessions had asked for
  `inputAudioTranscription`; none of the sessions without it failed, and the 3.8 models deliver the input transcript
  anyway, so the field is no longer sent (`liveInputTranscripts: true` in `settings.json` brings it back for models
  that need it). (2) Because the failure is not deterministic, the watchdog no longer just asks "please answer": it
  **relays the server's own transcript of your words as a text turn** (*The model did not answer that — sending your
  words as text…*), which brought an answer in every test; an empty `serverContent`, which precedes the first audio of
  an answer by a fraction of a second, extends the wait once. (2b) Now and then an utterance is lost outright — no
  `voiceActivity` echo, no transcript, and a later text turn is answered with `<no speech>`; since the echo and the
  transcript normally arrive within half a second of `activityEnd`, the application keeps the recording of the current
  utterance (up to 90 s) and sends it again as a fresh activity when neither has come after `liveResendS` (4) seconds
  (*The server did not take that in — sending it again…*), before the text relay. (3) With no `realtimeInputConfig` the server's own
  detection did not react to the probe's utterances at all (four sessions: no `voiceActivity`, no transcript, no
  answer), while an explicit configuration reacted immediately; when the app's detection is off, the setup now asks
  for `START_SENSITIVITY_HIGH`, `END_SENSITIVITY_LOW`, `prefixPaddingMs` 200 and `silenceDurationMs` =
  `liveVadEndMs`, and the (undocumented) `{"voiceActivity": {"type": "ACTIVITY_START" | "ACTIVITY_END",
  "audioOffset"}}` messages the server sends in both modes drive `Hearing you…`, the transcript entries and the
  watchdog exactly like the app's detector does.
- **Tools**: every tool call appears as a row in the transcript (running · done · failed · declined · cancelled).
  The assistant can do anything a user can do in the application (see *What it can do* below). By default it
  **acts on its own**: it loads data, builds maps, changes the look, exports figures and saves projects without
  asking, and only asks before replacing a whole project or map without being told to. Switch the setting *Acts
  on its own* off (Settings → Live AI) to get an approval card — **Allow / Allow all / Decline** — before every
  change; the shield in the pop-up header toggles the same thing for the current session. **Undo changes** appears
  once the session changed something.
- **The orb**: the circle button in the header (or *Live AI: voice orb* in the palette, or `livemini on`) melts the
  panel into a small glowing sphere at the lower right that keeps listening and talking. It is alive: it breathes
  while idle, its aura swells and a ring of voice bars dances with your voice (teal) or the assistant's (coral),
  ripples leave it on every syllable, electrons orbit it while it thinks (violet), a spinner runs while a tool works,
  and it pulses red on errors with the reason in a caption. The assistant's words appear in a caption bubble while
  it speaks. **Click** the sphere = start / stop talking; **double-click** or **right-click** = back to the panel;
  hovering shows two tiny buttons above it (open the panel, close). The *Live* toolbar button, `Ctrl+Shift+L` and
  the palette entries also bring the panel back while the orb is shown, and the orb expands by itself when a change
  needs approval. Panel and orb morph into each other (0.45 s).
- **Move it**: drag the panel by its header, or drag the orb itself, anywhere in the window; the positions are kept
  in Settings (`livePanelPos`, `liveOrbPos`, fractions of the window) and clamped when the window shrinks. Near the
  top edge the orb's two little buttons move below it; near the left edge its caption bubble opens to the right.
- **Show it the screen** (vision on demand): the camera button in the composer sends **one** still JPEG of the map
  canvas (or of the window when there is no map) to the model as an image, and the assistant can take one itself with
  the `look_at_screen` tool when you ask it to look at the map, a chart or the layout. There is never a video stream:
  a picture is taken only when you or the model ask for it, and the transcript notes each one (📷 *Sent a picture of
  the map canvas (1280×720, 140 KB)*).
- **History**: when the pop-up is closed, the project is saved or the application quits, the transcript is copied
  into the Assistant's conversation (AI page) as *Live session* turns, with the tools used under each answer, and so
  saved with the project like any other chat.
- **Close** (× in the header) ends the session and clears the transcript (after copying it into the chat).
- **Settings → Live AI**: Google Gemini API key (the same key as the AI Assistant's Gemini provider), model
  (`gemini-3.8-live-extended-thinking` by default, or `gemini-3.8-live`), voice, thinking level (extended-thinking
  models only), barge-in, Google Search grounding, *Acts on its own* (on by default).

## What it can do

The live assistant has the agent's typed tools (records, map, views, focus, pages, builds, clusters, merging,
OpenAlex, papers, reports) plus a layer that reaches **every** feature of the application:

| Tool | Effect |
|---|---|
| `get_ui_state` | What is on screen: project, file, records, map, analysis settings, view, page and tab, theme and look, search and selection, open preview or chart, running task, map history, report. |
| `load_data` | The Web of Science or Scopus sample, bibliographic files (WoS, Scopus, RIS, PubMed, CSV, OpenAlex, VOSviewer map/network) or a `.vosproj` project. |
| `save_project` | Saves the project (`.vosproj`); bare names go to `Documents\VOSStudio\Exports`. |
| `set_look` | Look preset, theme, hulls, cluster names, legend, labels, halos, link shape and count, inspector. |
| `export_figure` | The publication figure or the current view as PNG, SVG or PDF; returns the path. |
| `screenshot` | PNG of the window or of the map canvas; returns the path. |
| `open_page` | A page of the left panel and, optionally, one of its tabs. |
| `look_at_screen` | One still picture of the map canvas or of the window, sent to the model as an image (vision on demand; never a stream). |
| `write_report` | Drafts a whole report — several sections of 150–900 words each, grounded in the records, trends and cited papers — with the application's **text model** (Settings → AI Assistant), then adds it to the report; `edit_section` rewrites or removes one section, `get_report full=true` returns the exact text. |
| `list_commands` | The complete command reference for `run_commands`. |
| `show_compare` | Compare mode: `kind` periods / sources / thresholds, `layout` side_by_side / difference / none, the years, file names or numbers, or the two minimum weights; opens Trends › Compare, draws the figure in the main area and returns the document counts per side (difference: appearing / growing / stable / fading). |
| `run_commands` | Runs up to 40 application commands in order — the same commands as `--script` (see the script guide): data, analysis, views and pages, search and selection, trends and comparison (bursts, stability, sweep, difference maps, main paths, flows), geo, publishing, OpenAlex, cleaning, living maps, `wait`. Each command finishes (jobs, animations and screenshots included) before the next starts. |

`run_commands` goes through `App::runCommand`, a side-effect-free wrapper around the script interpreter: the
commands that would end the program, click or type for the user, open dialogs or drive the assistants themselves
(`quit`, `crashtest`, `click`, `keys`, `hover`, `settings`, `ai*`, `agent*`, `live*` …) are refused. Whether a batch
counts as a *change* (approval) is decided from its commands (`App::commandKind`). Files are written with absolute
paths or into `Documents\VOSStudio\Exports`, never elsewhere.

## How it works

```
src/core/live.h/.cpp     protocol (portable, unit-tested in tests/live_test.cpp)
  Options → setup message        model, response modality, voice, thinkingConfig, system instruction,
                                 function declarations (from ai::agentTools()), googleSearch (setting, off on
                                 free-tier keys), transcriptions,
                                 contextWindowCompression, sessionResumption
  textMessage / audioMessage     realtimeInput (text, or audio/pcm;rate=16000 in base64)
  activityStart/EndMessage       realtimeInput activity markers (client-side speech detection; the setup then carries
                                 realtimeInputConfig.automaticActivityDetection.disabled = true)
  SpeechDetector                 the client-side detector (unit-tested with synthetic audio)
  toolResponseMessage            functionResponses [{id, name, response: {output|error, scheduling?}}]
  parseServerMessage             setupComplete, modelTurn (audio + text; thought parts skipped), transcripts,
                                 turnComplete / interrupted / generationComplete, interactionStatus (top level or
                                 in serverContent), toolCall, toolCallCancellation, goAway, sessionResumptionUpdate,
                                 grounding metadata, usage, error
  Transcript                     ordered entries (user, model, tool, note); interim input transcripts merge in place
src/win/live.cpp         Windows layer
  WebSocket                      WinHTTP: GET + WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, receiver thread (assembles
                                 fragments), sender thread with a queue, events polled by the UI thread, WM_NULL wake-up;
                                 closing runs on a helper thread so the UI never waits
  Audio                          WASAPI shared mode. Capture 16 kHz mono int16 (AUTOCONVERTPCM when available,
                                 otherwise the mix format is down-mixed and linearly resampled), 40 ms chunks →
                                 audioMessage. Render 24 kHz from a sample queue with resampling to the device rate;
                                 flush() on `interrupted`. Half-duplex gating: zeros are sent while the speaker plays
                                 (plus 250 ms) unless barge-in is on.
  App::live*                     session state machine (see app.h LiveState), tool bridge, pop-up drawing
```

### Extended Thinking lifecycle

`gemini-3.8-live-extended-thinking` reasons in the background and speaks short fillers meanwhile. The client treats an
interaction as finished only when `interactionStatus` is `IDLE`; `turnComplete` alone ends an utterance, not the
task. All function declarations carry `behavior: "NON_BLOCKING"` for this model (required). The status line shows
`Thinking…` while the interaction is in progress and the model is silent.

### Tool bridge

Function calls are queued and run one at a time on the UI thread through `agentExecute()` — the same code path as the
Assistant's agent, so every tool, its undo snapshot and its job handling behave identically. `AgentRun::live` marks a
run as a live tool: `agentFinishTool` routes the result to `liveToolDone`, which sends the `toolResponse` and starts
the next queued call (deferred by one frame, because `agentExecute(agent.pending)` may still be on the stack).
Change tools honour the approval mode; declined tools return an error result so the model does not retry them.
A running tool survives a reconnection: its result is queued and sent after `setupComplete` on the new connection.
If the project changes underneath (the Assistant resets the agent), the tool is reported as interrupted.

### Frames and CPU

The overlay animates without redrawing the application. At the end of a full frame `Gfx::copyCapture(overlayCopy)`
keeps a GPU copy of the window *without* the overlay (a `CopyResource` of the back buffer, flushed first); while only
the orb or the panel's level meter moves — no input, no job, no other animation, no popup, dialog, tooltip or toast —
`App::frame` runs `frameOverlay` instead: the copy is drawn back, then only `drawLive` and the caption buttons, at the
rate the overlay asked for with `liveAnim(fps)` (12 fps while the orb merely breathes, 20–30 while someone speaks or
the mouse is near it, 60 during the panel↔orb morph). Words and sound arriving from the server mark the overlay
*dirty* (one overlay frame, at most 30 per second); tool calls and session changes still request a full frame, because
they may change anything. When nothing moves the message loop blocks as before (0 % CPU).

The same mechanism serves the map itself (1.9.3, *canvas layer cache*, see the README): the GPU render and the label
overlay are kept as a second copy (`Gfx::canvasCopy`) and drawn back while none of their inputs changed, so hovering
or working in a panel redraws only the chrome.

### "A system error occurred"

`gemini-3.8-live-extended-thinking` sometimes answers *a system error occurred while trying to…* instead of calling a
tool: the model's function call failed inside Google's service before anything reached the application (no tool row
appears, the application received no request and nothing went wrong on its side). It is a server-side hiccup of the
thinking model (the plain `gemini-3.8-live` model does it much less). The application now notices such a turn — a
model reply containing *system error*, *internal error*, *something went wrong* … with no tool call in the user's turn
— and sends one automatic nudge (*that error happened on your side, no tool was called; carry out my last request now
by calling the right tool*), once per user turn; the transcript shows the note. Repeating the request usually works.

### Reconnection

Every connection asks for `sessionResumption` and `contextWindowCompression`. A `goAway` schedules a reconnection
with the latest resume handle once the model is idle; an unexpected close while talking or waiting reconnects up to
three times in a row (the counter resets after two minutes of a stable connection). Authentication and argument
errors (close codes 1007/1008, upgrade failures) are shown in the status line, which is clickable to retry or to open
Settings.

## Checking it on Windows

Automated tests cover the protocol (`make test` → `live_test`). The Windows layer needs a machine with a microphone,
a speaker and a Gemini API key:

1. Settings → Live AI: paste the key, keep the default model. Open the pop-up (`Ctrl+Shift+L`): the status should
   read `Connecting…` and then `Ready — type a message`, with the note *Connected to Gemini 3.8 Live Thinking · text*.
2. Type `How many records are loaded?` after loading the sample: a `get_overview` row appears, then the answer.
3. Type `Build a keyword map with at least 5 occurrences`: an approval card appears (unless allow-all); *Allow*
   starts the build (`Building the map…`), the row turns green when done and the model reports the map.
4. Press the microphone: the session reconnects as a voice session (*Reconnected.*), the status shows `Listening`
   and the level meter moves. Say *switch to the density view*: the status reads `Hearing you…` while you talk and
   `Thinking…` about a second after you stop, the transcript shows your words, the view changes, the assistant
   answers by voice with the transcript in the pop-up. The mic meter reads `mic paused` while it speaks. If
   `Hearing you…` never appears while you talk, the microphone is too quiet for the detector: lower `liveVadDb` in
   `settings.json` (e.g. −55) or switch the detection back to the server's in Settings → Live AI.
5. While the assistant speaks, press the stop button: playback stops immediately and the entry ends with `…`.
6. Close the pop-up during a build: the build finishes normally, nothing is reported, the pop-up is gone.
7. Shrink to the orb (circle button in the header) while talking: the panel morphs into the sphere, the voice ring
   and aura move with your voice, the caption shows the assistant's words while it speaks, hovering shows the two
   buttons above it, and a double-click / right-click / the toolbar button / `Ctrl+Shift+L` bring the panel back
   with the full transcript.
9. Say *load the sample, build a keyword map and export it as a PNG*: with *Acts on its own* on, three tool rows run
   without an approval card and the assistant tells you the path of the file in `Documents\VOSStudio\Exports`.
8. `VOSStudio.exe --script tests\live.txt` with a script such as
   `sample` / `build` / `live How many clusters does the map have?` / `livewait` / `livelog` / `quit`
   writes the state and transcript to `script.log`.

Things to look at if something is off: Windows *Privacy → Microphone* must allow desktop apps; the key must be an
AI Studio key with access to the Live models; close code 1008 with `API key not valid` means the key is wrong,
HTTP 404 means the model name is not available for the key.

**"You exceeded your current quota" on the first connection** (close code 1011) does not mean you used anything.
Tested against the real service (September 2026, free-tier AI Studio key): the setup is accepted with the model, the
voice, the thinking level, all function declarations, the transcriptions, context compression and session
resumption — and refused with exactly this message as soon as the `googleSearch` tool is part of it. Grounding with
Google Search is not included in the free tier for Live sessions, and Google reports that as an exhausted quota
before `setupComplete`. VOSStudio handles it: when a setup with search on is refused for a quota, it switches
**Google Search grounding** off in Settings → Live AI, says so in the transcript and reconnects at once without it
(typed text that was waiting is still sent). Turn the toggle back on when the key's project has billing (Tier 1).
The WebSocket close frame can carry only 123 bytes, so the part of the message that names the quota is cut off;
if it happens with search already off, click **Diagnose** in the status line: it looks the model up with the key and
sends one tiny text request to `gemini-3.8-flash`, then writes a verdict into the transcript —
- text works, Live refused → the free tier of this project does not include the Live model (or its limit is 0):
  check *AI Studio → Rate limits* for the model, link a billing account (Tier 1) or try the other Live model;
- text refused too (full `limit: 0, model: …` message shown) → the project has no usable quota at all (new project not
  provisioned, free tier unavailable for the region/account, billing state): create a project in the Cloud console
  with the Generative Language API enabled and make a key for it in AI Studio.
The session also keeps its first turn light (the context sent with the setup is capped) so a large project never
trips a per-minute token limit by itself.
