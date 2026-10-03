#!/usr/bin/env python3
"""live_probe.py - end-to-end check of spoken turns against the Gemini Live API, done the way VOSStudio does it.

    GKEY=<your key> python3 live_probe.py question.wav [more.wav ...] [--server-vad] [--repeat 3] [--model gemini-3.8-live]

The WAV files play the part of the microphone: each is streamed as 16 kHz mono 16-bit PCM in 40 ms chunks, in real
time, one question after another in the same session. The setup message mirrors VOSStudio 1.9.6 (see
src/core/live.cpp setupJson: AUDIO replies with a voice, thinking level LOW on the Extended Thinking model, output
transcription, sliding-window compression, session resumption; no inputAudioTranscription - the 3.8 models transcribe
without it, and asking for it went together with unanswered turns in our tests; --input-transcripts adds it back).

Default = the application's own speech detection: activityStart, the audio, activityEnd, with the server's automatic
activity detection switched off. --server-vad = the server decides when the utterance has ended (configured explicitly
as the application does: START_SENSITIVITY_HIGH, END_SENSITIVITY_LOW, prefixPaddingMs 200, silenceDurationMs 900);
faint noise is streamed before and after the speech, as a microphone would.

--resend S and --relay S mirror the application's two-step watchdog. Step one: the server normally echoes
ACTIVITY_END and sends the transcript within half a second of activityEnd; when neither has come after S seconds
(default 4, like the application's liveResendS) the recording is sent again as a fresh activity - now and then the
server loses an utterance entirely and later reports "<no speech>" for it. Step two: if nothing has come back S
seconds later (default 6, liveWatchdogS; no audio, no text, no tool call, no turnComplete), the server's transcript
of the question is sent as a text turn. --resend 0 --relay 0 shows the raw behaviour of the server.

Every server event is printed with a timestamp: voiceActivity echoes, input transcription, empty serverContent (a
sign of life), first audio, output transcription, interactionStatus, turnComplete (with the undocumented
turnCompleteReason), tool calls, close code. The verdict for a question is ANSWERED when speech or an output
transcript arrived, RESENT when it took the second sending, RELAYED when only the relayed text was answered,
NO ANSWER otherwise. Exit code 0 when every question of every session was answered (one way or another).

Needs:  pip install websockets
Make a test WAV on Windows (16 kHz mono, any sentence) with PowerShell:
  Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
  $f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
  $s.SetOutputToWaveFile("question.wav", $f); $s.Speak("How many clusters does the map have?"); $s.Dispose()
Other rates and stereo files are converted here. The key is read from the environment only; nothing is written.
"""
import argparse
import asyncio
import base64
import json
import os
import random
import struct
import sys
import time
import wave

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets")

ENDPOINT = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="
RATE = 16000
CHUNK = RATE // 25  # 40 ms, as in the application


def load_pcm(path):
    """WAV -> list of int16 samples at 16 kHz mono (linear resampling, channels averaged)."""
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        s = [v / 32768.0 for v in struct.unpack("<%dh" % (len(raw) // 2), raw)]
    elif width == 1:
        s = [(b - 128) / 128.0 for b in raw]
    elif width == 4:
        s = [v / 2147483648.0 for v in struct.unpack("<%di" % (len(raw) // 4), raw)]
    else:
        sys.exit("unsupported sample width %d" % width)
    if ch > 1:
        s = [sum(s[i:i + ch]) / ch for i in range(0, len(s) - ch + 1, ch)]
    if rate != RATE:
        out, ratio, pos = [], rate / RATE, 0.0
        while pos + 1 < len(s):
            k = int(pos)
            fr = pos - k
            out.append(s[k] * (1 - fr) + s[k + 1] * fr)
            pos += ratio
        s = out
    return [int(max(-1.0, min(1.0, v)) * 32767) for v in s]


def chunks_of(pcm):
    out = [pcm[i:i + CHUNK] for i in range(0, len(pcm), CHUNK)]
    if out and len(out[-1]) < CHUNK:
        out[-1] = out[-1] + [0] * (CHUNK - len(out[-1]))
    return out


def audio_msg(samples):
    data = base64.b64encode(struct.pack("<%dh" % len(samples), *samples)).decode()
    return json.dumps({"realtimeInput": {"audio": {"data": data, "mimeType": "audio/pcm;rate=16000"}}})


def noise_chunk(rms):
    return [int(random.gauss(0, rms)) for _ in range(CHUNK)]


def setup_msg(a):
    """The application's setup (src/core/live.cpp setupJson), minus the tool declarations and Google Search."""
    if a.setup_file:
        setup = json.load(open(a.setup_file))
        return json.dumps({"setup": setup})
    gen = {"responseModalities": ["AUDIO"], "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": a.voice}}}}
    if "thinking" in a.model:
        gen["thinkingConfig"] = {"thinkingLevel": "LOW"}
    setup = {
        "model": "models/" + a.model,
        "generationConfig": gen,
        "systemInstruction": {"parts": [{"text": a.system}]},
        "outputAudioTranscription": {},
        "contextWindowCompression": {"slidingWindow": {}},
        "realtimeInputConfig": {"automaticActivityDetection": {"disabled": True} if not a.server_vad else {
            "disabled": False, "startOfSpeechSensitivity": "START_SENSITIVITY_HIGH", "endOfSpeechSensitivity": "END_SENSITIVITY_LOW",
            "prefixPaddingMs": 200, "silenceDurationMs": a.silence_ms}},
        "sessionResumption": {},
    }
    if a.input_transcripts:
        setup["inputAudioTranscription"] = {}
    return json.dumps({"setup": setup})


class Turn:
    def __init__(self, name):
        self.name = name
        self.heard = ""
        self.said = ""
        self.audio = 0
        self.calls = 0
        self.complete = False
        self.reason = ""
        self.relayed = False
        self.ack = False       # the server echoed ACTIVITY_END or sent a transcript: the utterance arrived
        self.resent = False    # the recording was sent a second time (it had not arrived)
        self.t_end = None      # when the question had been sent
        self.t_first = None    # first sign of an answer

    def answered(self):
        return self.audio > 0 or bool(self.said.strip()) or self.calls > 0

    def verdict(self):
        if self.answered():
            return "RELAYED" if self.relayed else "RESENT" if self.resent else "ANSWERED"
        return "NO ANSWER"


async def session(a, key, questions, idx):
    t0 = time.monotonic()

    def ev(*parts):
        print("%7.2f  %s" % (time.monotonic() - t0, " ".join(str(p) for p in parts)), flush=True)

    turns = [Turn(os.path.basename(q)) for q, _ in questions]
    cur = {"i": -1}
    life = {"seen": False}

    def turn():
        return turns[cur["i"]] if 0 <= cur["i"] < len(turns) else None

    print("--- session %d: %s, %s" % (idx, a.model, "server VAD" if a.server_vad else "client activity markers"))
    async with websockets.connect(ENDPOINT + key, max_size=None, ping_interval=None) as ws:
        await ws.send(setup_msg(a))
        first = json.loads(await ws.recv())
        if "setupComplete" not in first:
            ev("setup refused:", json.dumps(first)[:400])
            return turns
        ev("setupComplete")

        async def send_paced(msg, pace):
            await ws.send(msg)
            pace[0] += CHUNK / RATE
            await asyncio.sleep(max(0.0, pace[0] - time.monotonic()))

        async def relay_if_silent(t):
            if a.relay <= 0:
                return
            end = time.monotonic() + a.relay
            while time.monotonic() < end and not t.answered() and not t.complete:
                await asyncio.sleep(0.1)
            if not t.answered() and not t.complete:
                heard = t.heard.strip()
                if heard:
                    text = "(The user just said this by voice and the audio turn produced no answer. Answer it now, as if you had heard it: \"%s\")" % heard
                else:
                    text = "(The user has finished speaking and is waiting, but their words may not have reached you. If you heard them, answer now; if not, say out loud, in one short sentence, that you did not catch it and ask them to say it again.)"
                t.relayed = True
                await ws.send(json.dumps({"clientContent": {"turns": [{"role": "user", "parts": [{"text": text}]}], "turnComplete": True}}))
                ev("-> RELAY as text:", repr(text[:100]))

        async def wait_turn(t):
            end = time.monotonic() + a.observe
            while time.monotonic() < end and not t.complete:
                await asyncio.sleep(0.1)

        async def sender():
            pace = [time.monotonic()]
            if a.server_vad:
                end = time.monotonic() + a.preamble
                while time.monotonic() < end:
                    await send_paced(audio_msg(noise_chunk(a.noise)), pace)
            for qi, (path, pcm) in enumerate(questions):
                cur["i"] = qi
                t = turns[qi]
                life["seen"] = False
                pace[0] = time.monotonic()
                if a.server_vad:
                    ev("-> speaking", t.name)
                    for c in chunks_of(pcm):
                        await send_paced(audio_msg(c), pace)
                    t.t_end = time.monotonic()
                    ev("-> speech over; faint noise follows")
                    quiet = asyncio.create_task(relay_if_silent(t))
                    end = time.monotonic() + a.observe
                    while time.monotonic() < end and not t.complete:
                        await send_paced(audio_msg(noise_chunk(a.noise)), pace)
                    await quiet
                else:
                    await ws.send(json.dumps({"realtimeInput": {"activityStart": {}}}))
                    ev("-> activityStart", t.name)
                    for c in chunks_of(pcm):
                        await send_paced(audio_msg(c), pace)
                    await ws.send(json.dumps({"realtimeInput": {"activityEnd": {}}}))
                    t.t_end = time.monotonic()
                    ev("-> activityEnd")
                    if a.resend > 0:  # the application's step one: no echo, no transcript -> the utterance is sent again
                        end = time.monotonic() + a.resend
                        while time.monotonic() < end and not t.ack and not t.answered() and not t.complete:
                            await asyncio.sleep(0.1)
                        if not t.ack and not t.answered() and not t.complete:
                            t.resent = True
                            await ws.send(json.dumps({"realtimeInput": {"activityStart": {}}}))
                            for i in range(0, len(pcm), CHUNK * 5):  # 200 ms per message, no pacing, as the application does
                                await ws.send(audio_msg(pcm[i:i + CHUNK * 5]))
                            await ws.send(json.dumps({"realtimeInput": {"activityEnd": {}}}))
                            ev("-> RESENT the utterance (no ACTIVITY_END echo, no transcript after %.0f s)" % a.resend)
                    await relay_if_silent(t)
                    await wait_turn(t)
                if not t.complete:
                    ev("!! the turn is still open after %.0f s" % a.observe)
                await asyncio.sleep(0.5)

        async def receiver():
            try:
                async for raw in ws:
                    m = json.loads(raw)
                    if a.verbose:
                        ev("<-", json.dumps(m)[:300])
                    t = turn()
                    if "voiceActivity" in m:
                        ev("voiceActivity", json.dumps(m["voiceActivity"]))
                        if t and m["voiceActivity"].get("type") == "ACTIVITY_END":
                            t.ack = True
                        continue
                    if "toolCall" in m:
                        calls = m["toolCall"].get("functionCalls", [])
                        ev("toolCall", ", ".join(c.get("name", "?") for c in calls))
                        if t:
                            t.calls += len(calls)
                        # the probe has no application behind it: answer every call with a neutral result
                        await ws.send(json.dumps({"toolResponse": {"functionResponses": [
                            {"id": c.get("id"), "name": c.get("name"), "response": {"output": "The probe has no project loaded; answer in words."}} for c in calls]}}))
                        continue
                    sc = m.get("serverContent")
                    if sc is None:
                        if m and not ({"usageMetadata", "sessionResumptionUpdate"} >= set(m.keys())):
                            ev("<-", json.dumps(m)[:300])
                        continue
                    if "inputTranscription" in sc:
                        txt = sc["inputTranscription"].get("text", "")
                        ev("heard", repr(txt))
                        if t:
                            t.ack = True
                            if not t.answered():
                                t.heard += txt
                    if "interimInputTranscription" in sc:
                        ev("heard (interim)", repr(sc["interimInputTranscription"].get("text", "")))
                    if "outputTranscription" in sc:
                        txt = sc["outputTranscription"].get("text", "")
                        if t:
                            if not t.said:
                                ev("said", repr(txt))
                            t.said += txt
                    for p in sc.get("modelTurn", {}).get("parts", []):
                        if "inlineData" in p:
                            n = len(base64.b64decode(p["inlineData"]["data"]))
                            if t:
                                if t.audio == 0:
                                    t.t_first = time.monotonic()
                                    ev("first audio (%d bytes), %.1f s after the question" % (n, t.t_first - (t.t_end or t.t_first)))
                                t.audio += n
                        elif p.get("text") and not p.get("thought"):
                            ev("text", repr(p["text"][:120]))
                            if t:
                                t.said += p["text"]
                    for k in ("interactionStatus", "waitingForInput", "interrupted", "generationComplete", "turnCompleteReason"):
                        if k in sc:
                            ev(k, sc[k])
                    if not sc and t and not t.answered() and not life["seen"]:
                        life["seen"] = True
                        ev("empty serverContent (a sign of life)")
                    if sc.get("turnComplete"):
                        if t:
                            t.complete = True
                            t.reason = sc.get("turnCompleteReason", "")
                            ev("turnComplete: %s  audio=%d bytes  said=%r" % (t.verdict(), t.audio, t.said.strip()[:100]))
                        else:
                            ev("turnComplete (no question pending)")
            except websockets.ConnectionClosed as e:
                ev("closed", e.code, repr(e.reason))

        s = asyncio.create_task(sender())
        r = asyncio.create_task(receiver())
        done, _ = await asyncio.wait({s, r}, return_when=asyncio.FIRST_COMPLETED)
        if r not in done:
            try:
                await asyncio.wait_for(r, timeout=2)
            except asyncio.TimeoutError:
                pass
        for task in (s, r):
            if not task.done():
                task.cancel()
    return turns


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="+", help="spoken questions, WAV (16 kHz mono 16-bit preferred; others are converted)")
    ap.add_argument("--model", default="gemini-3.8-live-extended-thinking")
    ap.add_argument("--voice", default="Puck")
    ap.add_argument("--system", default="You are the live assistant of VOS Studio, a desktop application for concept maps. Answer briefly.")
    ap.add_argument("--server-vad", action="store_true", help="the server's automatic activity detection (setting liveClientVad off)")
    ap.add_argument("--silence-ms", type=int, default=900, help="server VAD: silenceDurationMs (the application sends liveVadEndMs)")
    ap.add_argument("--preamble", type=float, default=1.0, help="server VAD: seconds of faint noise before the first question")
    ap.add_argument("--noise", type=float, default=30.0, help="server VAD: RMS of the faint noise between questions (16-bit units)")
    ap.add_argument("--input-transcripts", action="store_true", help="ask for inputAudioTranscription explicitly (VOSStudio <= 1.9.5)")
    ap.add_argument("--resend", type=float, default=4.0, help="watchdog step one: send the utterance again when neither an ACTIVITY_END echo nor a transcript came within this many seconds (0 = never)")
    ap.add_argument("--relay", type=float, default=6.0, help="watchdog step two: relay the transcript as text after this many further silent seconds (0 = never)")
    ap.add_argument("--repeat", type=int, default=1, help="number of sessions (the server fault is intermittent)")
    ap.add_argument("--observe", type=float, default=20.0, help="seconds to wait for each answer")
    ap.add_argument("--setup-file", default="", help="use this JSON object as the setup instead of the built-in one")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every raw server message (truncated)")
    a = ap.parse_args()
    key = os.environ.get("GKEY") or os.environ.get("GEMINI_API_KEY")
    if not key:
        sys.exit("set GKEY (or GEMINI_API_KEY) in the environment")
    questions = [(p, load_pcm(p)) for p in a.wav]
    for p, pcm in questions:
        print("%s: %.2f s of speech" % (p, len(pcm) / RATE))
    results = []
    for i in range(1, a.repeat + 1):
        turns = asyncio.run(session(a, key, questions, i))
        results.append(turns)
        for t in turns:
            print("    %-24s %-10s heard=%r said=%r" % (t.name, t.verdict(), t.heard.strip()[:60], t.said.strip()[:80]))
    flat = [t for ts in results for t in ts]
    ok = sum(1 for t in flat if t.answered())
    relayed = sum(1 for t in flat if t.answered() and t.relayed)
    resent = sum(1 for t in flat if t.answered() and t.resent and not t.relayed)
    print("SUMMARY: %d/%d questions answered (%d after sending the utterance again, %d by relaying the transcript as text) in %d session(s)" % (ok, len(flat), resent, relayed, len(results)))
    sys.exit(0 if flat and ok == len(flat) else 1)


if __name__ == "__main__":
    main()
