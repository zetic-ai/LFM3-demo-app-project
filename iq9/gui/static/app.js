"use strict";
const $ = (id) => document.getElementById(id);
const escapeHtml = (value) =>
  String(value ?? "").replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
const uid = () => crypto.randomUUID();
const defaults = {
  context: 2048,
  max_tokens: 256,
  temperature: 0.1,
  top_p: 0.95,
  top_k: 50,
  seed: 1,
  repetition_penalty: 1.1,
  max_audio_seconds: 30,
  set_size: 16,
  calibrate: true,
  cache_prompt: true,
  autoplay: true,
  system: "",
};
let settings = { ...defaults, ...readStore("lfm3.settings", {}) };
let sessions = readStore("lfm3.sessions", []),
  session = null,
  info = null,
  engine = "zetic",
  busy = false,
  attachments = [],
  questions = [],
  latestPair = null;
let audioContext,
  audioNext = 0,
  audioSources = [],
  redrawPending = false;
function readStore(key, fallback) {
  try {
    return JSON.parse(localStorage.getItem(key)) ?? fallback;
  } catch {
    return fallback;
  }
}
function toast(message) {
  $("toast").textContent = message;
  $("toast").hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => ($("toast").hidden = true), 6500);
}
function status(message, error = false) {
  $("status-text").textContent = message;
  $("runtime-status").classList.toggle("error", error);
}
async function api(url, body) {
  const r = await fetch(
    url,
    body === undefined
      ? {}
      : {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        },
  );
  const text = await r.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    throw Error(text || `HTTP ${r.status}`);
  }
  if (!r.ok) throw Error(data.error || text);
  return data;
}
function model() {
  return info?.models.find((m) => m.id === $("model").value);
}
function setBusy(value) {
  busy = value;
  $("send").hidden = value;
  $("stop").hidden = !value;
  $("runtime-status").classList.toggle("busy", value);
  ["model", "compare", "prepare", "new-chat", "attach", "samples-open"].forEach(
    (id) => ($(id).disabled = value),
  );
  document
    .querySelectorAll("[data-engine]")
    .forEach((b) => (b.disabled = value));
}
function save() {
  if (!session) return;
  session.updated = Date.now();
  session.model = $("model").value;
  session.engine = engine;
  session.questions = questions;
  session.title =
    session.turns[0]?.user.text?.slice(0, 42) || model()?.name + " session";
  sessions = [session, ...sessions.filter((s) => s.id !== session.id)].slice(
    0,
    30,
  );
  try {
    localStorage.setItem("lfm3.sessions", JSON.stringify(sessions));
  } catch {
    toast(
      "Browser storage is full. Export this conversation before clearing old sessions.",
    );
  }
  renderRecents();
}
function newSession() {
  if (busy) return;
  stopAudio();
  session = {
    id: uid(),
    title: "New conversation",
    turns: [],
    updated: Date.now(),
  };
  attachments = [];
  $("prompt").value = "";
  latestPair = null;
  renderMessages();
  renderAttachments();
  renderRecents();
  renderChart();
}
function restoreComparison() {
  latestPair = null;
  for (const turn of session.turns) {
    if (!turn.compare) continue;
    const z = turn.results.find(
      (r) => r.engine === "zetic" && !r.error && !r.pending,
    );
    const l = turn.results.find(
      (r) => r.engine === "llama" && !r.error && !r.pending,
    );
    if (z && l) latestPair = { zetic: z, llama: l };
  }
  renderChart(Boolean(latestPair));
}
function renderRecents() {
  $("recents").replaceChildren();
  for (const s of sessions.slice(0, 6)) {
    const b = document.createElement("button");
    b.className = "recent" + (s.id === session?.id ? " active" : "");
    b.textContent = s.title || "Conversation";
    b.onclick = () => {
      if (busy) return;
      stopAudio();
      session = structuredClone(s);
      attachments = [];
      $("prompt").value = "";
      if (info.models.some((m) => m.id === s.model)) $("model").value = s.model;
      engine = s.engine || "zetic";
      questions = s.questions || [];
      updateModel();
      renderMessages();
      renderAttachments();
      renderRecents();
      restoreComparison();
    };
    $("recents").append(b);
  }
}
function markdown(text) {
  return escapeHtml(text)
    .replace(/```(?:\w+)?\n([\s\S]*?)```/g, "<pre><code>$1</code></pre>")
    .replace(/\*\*([^*\n]+)\*\*/g, "<strong>$1</strong>")
    .replace(/`([^`\n]+)`/g, "<code>$1</code>");
}
function mediaHtml(items) {
  return (items || [])
    .map((a) =>
      a.type === "audio"
        ? `<audio controls preload="metadata" src="${escapeHtml(a.url)}" aria-label="${escapeHtml(a.name)}"></audio>`
        : `<img src="${escapeHtml(a.url)}" alt="${escapeHtml(a.name)}">${a.type === "video" ? "<span>Video frames · 2 fps</span>" : ""}`,
    )
    .join("");
}
function renderAttachments() {
  $("attachment-list").innerHTML = attachments
    .map(
      (a, i) =>
        `<div class="attachment-chip">${a.type === "image" || a.type === "video" ? `<img src="${escapeHtml(a.url)}" alt="">` : a.type === "audio" ? `<audio controls src="${escapeHtml(a.url)}" preload="metadata"></audio>` : ""}<span>${escapeHtml(a.name)}</span><button data-remove="${i}" aria-label="Remove attachment">×</button></div>`,
    )
    .join("");
  $("attachment-list")
    .querySelectorAll("[data-remove]")
    .forEach(
      (b) =>
        (b.onclick = () => {
          attachments.splice(Number(b.dataset.remove), 1);
          renderAttachments();
        }),
    );
}
function probabilities(result) {
  return (result.answers || [])
    .map(
      (a) =>
        `<section class="answer-question"><span class="tag">${escapeHtml(a.type.toUpperCase())}${a.type === "score" ? (result.engine === "zetic" ? " · MEDIAN LEVEL" : " · EXPECTED INDEX") : ""}</span><h4>${escapeHtml(a.instructions)}</h4><div class="decision-selected">${escapeHtml(Array.isArray(a.choices) && a.choices.length ? a.choices.join(", ") : a.selected)}</div>${a.probabilities.map((p) => `<div class="prob-row"><span>${escapeHtml(p.label)}</span><div class="bar-track"><div class="bar zetic" style="width:${Math.max(0, Math.min(100, p.probability * 100))}%"></div></div><span>${(p.probability * 100).toFixed(1)}%</span></div>`).join("")}</section>`,
    )
    .join("");
}
function metricsHtml(r) {
  const m = r.metrics || {},
    parts = [];
  if (m.ttft_ms > 0)
    parts.push(
      `${r.answers ? "Decision" : "First response"} <b>${m.ttft_ms.toFixed(0)} ms</b>`,
    );
  if (m.tps > 0) parts.push(`<b>${m.tps.toFixed(1)}</b> tok/s`);
  if (m.first_audio_ms > 0)
    parts.push(`First audio <b>${m.first_audio_ms.toFixed(0)} ms</b>`);
  if (m.realtime_x > 0)
    parts.push(`<b>${m.realtime_x.toFixed(2)}×</b> realtime`);
  if (r.load_ms !== undefined)
    parts.push(`Load <b>${r.load_ms.toFixed(0)} ms</b>`);
  return parts.join("<span>·</span>");
}
function renderMessages() {
  if (!session) return;
  $("welcome").hidden = session.turns.length > 0;
  $("messages").innerHTML = session.turns
    .map(
      (turn, ti) =>
        `<article class="turn"><div class="user-message ${turn.user.text?.length > 1000 ? "long" : ""}">${escapeHtml(turn.user.text || "Analyze the attached media")}${turn.user.attachments?.length ? `<div class="message-attachments">${mediaHtml(turn.user.attachments)}</div>` : ""}</div><div class="answers ${turn.compare ? "compare" : ""}">${turn.results.map((r, ri) => `<section class="answer ${escapeHtml(r.engine)} ${r.error ? "error" : ""}"><div class="answer-header"><span>${r.engine === "zetic" ? "ZETIC ENGINE" : "llama.cpp"}</span><small>${r.pending ? "RUNNING" : r.error ? "ERROR" : r.cancelled ? "STOPPED" : escapeHtml(r.backend || "")}</small></div><div class="answer-body">${r.error ? escapeHtml(r.error) : r.answers ? probabilities(r) : markdown(r.text || "")}${r.pending && !r.text ? '<div class="typing"><span></span><span></span><span></span></div>' : ""}</div>${r.audio_url ? `<audio controls src="${escapeHtml(r.audio_url)}" preload="metadata"></audio>` : ""}${!r.pending && !r.error ? `<div class="answer-metrics">${metricsHtml(r)}</div><div class="answer-footer"><button data-copy="${ti},${ri}">Copy</button>${r.audio_url ? `<a class="text-button" href="${escapeHtml(r.audio_url)}" download>Save WAV ↓</a>` : ""}${turn.compare && !r.answers ? `<button data-select="${ti},${ri}">${turn.selected === ri ? "✓ Continuing with this" : "Continue with this"}</button>` : ""}${ti === session.turns.length - 1 && !turn.compare ? `<button data-retry="${ti}">Regenerate</button>` : ""}<details><summary>Details</summary><pre>${escapeHtml(JSON.stringify({ model: r.model, backend: r.backend, settings: r.settings, metrics: r.metrics, usage: r.usage, passes: r.passes, total_ms: r.total_ms, wall_ms: r.wall_ms, score_semantics: r.score_semantics, pipeline: r.pipeline }, null, 2))}</pre></details></div>` : ""}</section>`).join("")}</div></article>`,
    )
    .join("");
  $("messages")
    .querySelectorAll("[data-copy]")
    .forEach(
      (b) =>
        (b.onclick = async () => {
          const [t, r] = b.dataset.copy.split(",").map(Number),
            a = session.turns[t].results[r];
          await navigator.clipboard.writeText(
            a.answers ? JSON.stringify(a.answers, null, 2) : a.text,
          );
          toast("Copied");
        }),
    );
  $("messages")
    .querySelectorAll("[data-select]")
    .forEach(
      (b) =>
        (b.onclick = () => {
          if (busy) return;
          const [t, r] = b.dataset.select.split(",").map(Number);
          if (t !== session.turns.length - 1) {
            toast(
              "Start a new comparison to change the continuation after later messages.",
            );
            return;
          }
          session.turns[t].selected = r;
          save();
          renderMessages();
        }),
    );
  $("messages")
    .querySelectorAll("[data-retry]")
    .forEach(
      (b) =>
        (b.onclick = () => {
          if (busy) return;
          const turn = session.turns.pop();
          $("prompt").value = turn.user.text;
          attachments = turn.user.attachments || [];
          renderAttachments();
          send();
        }),
    );
}
function scheduleRender() {
  if (redrawPending) return;
  redrawPending = true;
  requestAnimationFrame(() => {
    redrawPending = false;
    const c = $("conversation"),
      atBottom = c.scrollHeight - c.scrollTop - c.clientHeight < 180;
    renderMessages();
    if (atBottom) c.scrollTop = c.scrollHeight;
  });
}
function history() {
  const out = [];
  if (settings.system.trim())
    out.push({ role: "system", text: settings.system });
  for (const t of session.turns) {
    out.push({
      role: "user",
      text: t.user.text,
      attachments: t.user.attachments || [],
    });
    const r = t.results[t.selected ?? 0];
    if (r && !r.pending && !r.error && !r.answers)
      out.push({
        role: "assistant",
        text: r.text || "",
        generated_id: r.generated_id || "",
      });
  }
  return out;
}
function requestBody() {
  let state = $("prompt").value;
  if ($("json-state").checked && model()?.kind === "decision")
    state = JSON.parse(state);
  return {
    model: $("model").value,
    engines: $("compare").checked ? ["zetic", "llama"] : [engine],
    settings: { ...settings },
    output: model()?.kind === "decision" ? "text" : $("output").value,
    messages: history(),
    state,
    questions: structuredClone(questions),
    attachments: structuredClone(attachments),
  };
}
async function send() {
  if (busy || !info) return;
  if (!$("prompt").value.trim() && !attachments.length) {
    toast("Enter a message or choose a sample.");
    return;
  }
  let q;
  try {
    q = requestBody();
  } catch {
    toast("Input is not valid JSON.");
    return;
  }
  stopAudio();
  if (settings.autoplay) {
    audioContext ??= new (window.AudioContext || window.webkitAudioContext)();
    await audioContext.resume();
  }
  const user = {
      text: $("prompt").value,
      attachments: structuredClone(attachments),
    },
    turn = {
      user,
      results: [],
      compare: $("compare").checked,
      selected: 0,
      model: $("model").value,
    };
  session.turns.push(turn);
  q.messages = history();
  q.attachments = structuredClone(attachments);
  q.state = model().kind === "decision" ? q.state : undefined;
  $("prompt").value = "";
  attachments = [];
  renderAttachments();
  setBusy(true);
  renderMessages();
  $("conversation").scrollTop = $("conversation").scrollHeight;
  status("Preparing your request…");
  try {
    const r = await fetch("/api/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(q),
    });
    if (!r.ok) {
      const text = await r.text();
      let data;
      try {
        data = JSON.parse(text);
      } catch {}
      throw Error(data?.error || text);
    }
    const reader = r.body.getReader(),
      decoder = new TextDecoder();
    let buffer = "";
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      let i;
      while ((i = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, i);
        buffer = buffer.slice(i + 1);
        if (!line) continue;
        const ev = JSON.parse(line);
        if (ev.event === "complete") {
          if (ev.cancelled) status("Stopped");
          continue;
        }
        let result = turn.results.find((a) => a.engine === ev.engine);
        if (ev.event === "run_start") {
          result = {
            engine: ev.engine,
            pending: true,
            text: "",
            model: q.model,
          };
          turn.results.push(result);
          audioNext = 0;
        } else if (ev.event === "status") status(ev.message);
        else if (ev.event === "loaded") {
          if (result) result.load_ms = ev.load_ms;
          status(
            `${ev.engine === "zetic" ? "ZETIC" : "llama.cpp"} · running ${model().name}…`,
          );
        } else if (ev.event === "text") {
          result.text += ev.delta;
        } else if (ev.event === "audio") {
          if (settings.autoplay) playPcm(ev.pcm, ev.rate);
        } else if (ev.event === "done") {
          Object.assign(result, ev, { pending: false });
          if (result.finish === "cancelled") {
            result.cancelled = true;
            stopAudio();
          }
          save();
        } else if (ev.event === "error") {
          Object.assign(result, { pending: false, error: ev.message });
          status(ev.message, true);
        } else if (ev.event === "cancelled") {
          if (result)
            Object.assign(result, { pending: false, cancelled: true });
          stopAudio();
        }
        scheduleRender();
      }
    }
    for (const r of turn.results) r.pending = false;
    if (turn.compare) {
      const z = turn.results.find((r) => r.engine === "zetic" && !r.error),
        l = turn.results.find((r) => r.engine === "llama" && !r.error);
      if (z && l) {
        latestPair = { zetic: z, llama: l };
        renderChart(true);
      }
      turn.selected = Math.max(
        0,
        turn.results.findIndex((r) => r.engine === "zetic" && !r.error),
      );
    }
    if (turn.results.some((r) => r.cancelled)) status("Stopped");
    else if (!turn.results.some((r) => r.error))
      status("Ready · model stays loaded for the next request");
  } catch (e) {
    turn.results.push({ engine, error: e.message, pending: false });
    status(e.message, true);
    toast(e.message);
  } finally {
    setBusy(false);
    save();
    renderMessages();
    $("conversation").scrollTop = $("conversation").scrollHeight;
    $("prompt").focus();
  }
}
function stopAudio() {
  document.querySelectorAll("audio").forEach((a) => a.pause());
  for (const source of audioSources) {
    try {
      source.stop();
    } catch {}
  }
  audioSources = [];
  audioNext = 0;
}
function playPcm(encoded, rate) {
  if (!audioContext) return;
  const bytes = Uint8Array.from(atob(encoded), (c) => c.charCodeAt(0)),
    buffer = audioContext.createBuffer(1, bytes.length / 2, rate),
    samples = buffer.getChannelData(0),
    view = new DataView(bytes.buffer);
  for (let i = 0; i < samples.length; i++)
    samples[i] = view.getInt16(i * 2, true) / 32768;
  const source = audioContext.createBufferSource();
  source.buffer = buffer;
  source.connect(audioContext.destination);
  audioNext = Math.max(audioContext.currentTime + 0.03, audioNext);
  source.start(audioNext);
  audioNext += buffer.duration;
  audioSources.push(source);
  source.onended = () => {
    audioSources = audioSources.filter((s) => s !== source);
  };
}
document.addEventListener(
  "play",
  (ev) => {
    if (ev.target instanceof HTMLMediaElement) {
      stopStreaming();
      document.querySelectorAll("audio").forEach((a) => {
        if (a !== ev.target) a.pause();
      });
    }
  },
  true,
);
function stopStreaming() {
  for (const source of audioSources) {
    try {
      source.stop();
    } catch {}
  }
  audioSources = [];
  audioNext = 0;
}
function updateModel() {
  const m = model();
  if (!m) return;
  $("decision-panel").hidden = m.kind !== "decision";
  $("output").disabled =
    m.kind === "decision" || !m.capabilities.includes("speech");
  if ($("output").disabled) $("output").value = "text";
  $("model-caption").textContent =
    `${m.name} · Context ${Number(settings.context).toLocaleString()}`;
  document
    .querySelectorAll("[data-engine]")
    .forEach((b) => b.classList.toggle("active", b.dataset.engine === engine));
  $("prompt").placeholder =
    m.kind === "decision"
      ? "Enter the state to evaluate, or attach media…"
      : "Message LFM3…";
  if (m.kind === "decision" && !questions.length)
    questions = [defaultQuestion()];
  renderQuestions();
  renderSampleCards();
  status(`Ready · ${m.name} · ${engine === "zetic" ? "ZETIC" : "llama.cpp"}`);
  if (info.device_error) status(info.device_error, true);
}
function defaultQuestion() {
  return {
    id: "q_" + uid().slice(0, 8),
    type: "noul",
    instructions: "Is this request asking for a refund?",
    criteria: [],
    multiple: false,
  };
}
function renderQuestions() {
  $("questions").innerHTML = questions
    .map(
      (q, i) =>
        `<section class="question-editor"><header><span>QUESTION ${i + 1}</span><select data-q-type="${i}" aria-label="Question type">${["noul", "choice", "score"].map((t) => `<option value="${t}" ${q.type === t ? "selected" : ""}>${t[0].toUpperCase() + t.slice(1)}</option>`).join("")}</select><button class="icon-button" data-q-delete="${i}" aria-label="Remove question">×</button></header><textarea data-q-text="${i}" aria-label="Question instructions" rows="2">${escapeHtml(q.instructions)}</textarea>${q.type === "noul" ? '<span class="tag">true / false</span>' : `${q.criteria.map((c, j) => `<div class="option-editor"><input data-option="${i},${j},label" aria-label="Option label" placeholder="Label" value="${escapeHtml(c.label)}">${q.type === "choice" ? `<input class="description" data-option="${i},${j},description" aria-label="Option description" placeholder="Description" value="${escapeHtml(c.description || "")}">` : ""}<button data-option-delete="${i},${j}" aria-label="Remove option">×</button></div>`).join("")}<button class="add-option" data-option-add="${i}">＋ Add ${q.type === "score" ? "level" : "option"}</button>${q.type === "choice" ? `<label class="multiple"><input type="checkbox" data-q-multiple="${i}" ${q.multiple ? "checked" : ""}> Allow multiple selections · ZETIC</label>` : ""}`}</section>`,
    )
    .join("");
  $("questions")
    .querySelectorAll("[data-q-type]")
    .forEach(
      (el) =>
        (el.onchange = () => {
          const q = questions[Number(el.dataset.qType)];
          q.type = el.value;
          if (q.type !== "noul" && !q.criteria.length)
            q.criteria =
              q.type === "score"
                ? [{ label: "low" }, { label: "medium" }, { label: "high" }]
                : [
                    { label: "yes", description: "" },
                    { label: "no", description: "" },
                  ];
          renderQuestions();
        }),
    );
  $("questions")
    .querySelectorAll("[data-q-text]")
    .forEach(
      (el) =>
        (el.oninput = () =>
          (questions[Number(el.dataset.qText)].instructions = el.value)),
    );
  $("questions")
    .querySelectorAll("[data-q-delete]")
    .forEach(
      (el) =>
        (el.onclick = () => {
          questions.splice(Number(el.dataset.qDelete), 1);
          renderQuestions();
        }),
    );
  $("questions")
    .querySelectorAll("[data-option]")
    .forEach(
      (el) =>
        (el.oninput = () => {
          const [i, j, key] = el.dataset.option.split(",");
          questions[i].criteria[j][key] = el.value;
        }),
    );
  $("questions")
    .querySelectorAll("[data-option-delete]")
    .forEach(
      (el) =>
        (el.onclick = () => {
          const [i, j] = el.dataset.optionDelete.split(",");
          questions[i].criteria.splice(Number(j), 1);
          renderQuestions();
        }),
    );
  $("questions")
    .querySelectorAll("[data-option-add]")
    .forEach(
      (el) =>
        (el.onclick = () => {
          questions[Number(el.dataset.optionAdd)].criteria.push({
            label: "",
            description: "",
          });
          renderQuestions();
        }),
    );
  $("questions")
    .querySelectorAll("[data-q-multiple]")
    .forEach(
      (el) =>
        (el.onchange = () =>
          (questions[Number(el.dataset.qMultiple)].multiple = el.checked)),
    );
}
function renderSampleCards() {
  if (!info) return;
  const icons = {
    cats: "▧",
    jfk: "≋",
    summary: "≡",
    tts: "♫",
    voice: "◉",
    ticket: "☷",
    d1image: "▦",
    d1audio: "≋",
  };
  const cards = (items) =>
    items
      .map(
        (s) =>
          `<button class="sample-card" data-sample="${s.id}"><span class="sample-icon">${icons[s.id] || "✳"}</span><span class="sample-info"><strong>${escapeHtml(s.title)}</strong><small>${escapeHtml(s.subtitle)}</small></span><span class="sample-arrow">↗</span></button>`,
      )
      .join("");
  const relevant = info.samples.filter((s) =>
    model()?.kind === "decision"
      ? s.model.startsWith("d1")
      : !s.model.startsWith("d1"),
  );
  $("sample-grid").innerHTML = cards(relevant.slice(0, 4));
  $("sample-dialog-grid").innerHTML = cards(info.samples);
  document
    .querySelectorAll("[data-sample]")
    .forEach((b) => (b.onclick = () => loadSample(b.dataset.sample)));
}
async function loadSample(id) {
  if (busy) return;
  try {
    status("Preparing sample…");
    const sample = await api("/api/sample/" + id);
    $("model").value = sample.model;
    if (sample.questions) questions = sample.questions;
    $("prompt").value = sample.text || "";
    attachments = sample.attachments || [];
    $("output").value = sample.output || "text";
    updateModel();
    renderAttachments();
    $("samples-dialog").close();
    $("prompt").focus();
    inputCount();
  } catch (e) {
    toast(e.message);
    status(e.message, true);
  }
}
function chartOptions() {
  const items = info.reference.items.map((a, i) => ({ key: "ref:" + i, ...a }));
  if (latestPair) {
    const z = latestPair.zetic,
      l = latestPair.llama;
    for (const [key, label, unit, lower] of [
      [
        "ttft_ms",
        z.answers ? "Decision latency" : "First response",
        "ms",
        true,
      ],
      ["tps", "Token generation", "tok/s", false],
      ["first_audio_ms", "First audio", "ms", true],
      ["realtime_x", "Speech speed", "× realtime", false],
    ]) {
      if (z.metrics?.[key] > 0 && l.metrics?.[key] > 0)
        items.unshift({
          key: "live:" + key,
          label: "Last comparison · " + label,
          metric: label,
          unit,
          lower_better: lower,
          zetic: z.metrics[key],
          llama: l.metrics[key],
          conditions: `${z.model}; identical input and settings; engine loading excluded. ${key === "ttft_ms" ? "App request to first text/audio event; D1 measures request to decisions." : ""}`,
        });
    }
  }
  return items;
}
function renderChart(preferLive = false) {
  if (!info) return;
  const items = chartOptions(),
    previous = $("chart-select").value;
  $("chart-select").innerHTML = items
    .map((i) => `<option value="${i.key}">${escapeHtml(i.label)}</option>`)
    .join("");
  if (!preferLive && items.some((i) => i.key === previous))
    $("chart-select").value = previous;
  if (preferLive && items.some((i) => i.key === "live:ttft_ms"))
    $("chart-select").value = "live:ttft_ms";
  const a = items.find((i) => i.key === $("chart-select").value) || items[0];
  const ratio = a.lower_better ? a.llama / a.zetic : a.zetic / a.llama;
  const isLive = a.key.startsWith("live:");
  $("chart-mode").textContent = isLive ? "LAST COMPARISON" : "REFERENCE";
  $("speed-label").textContent =
    ratio >= 1
      ? a.lower_better
        ? "Faster " + a.metric.toLowerCase()
        : "Faster " + a.metric.toLowerCase()
      : "ZETIC / llama.cpp";
  $("speed-value").innerHTML = ratio.toFixed(1) + "<span>×</span>";
  $("speed-context").textContent = isLive
    ? `${info.models.find((m) => m.id === latestPair.zetic.model)?.name} · measured comparison`
    : info.reference.title;
  $("speed-caption").textContent = isLive
    ? "Latest comparison. Model loading excluded."
    : "Recorded IQ9 results · " + info.reference.date;
  const max = Math.max(a.zetic, a.llama);
  $("chart").innerHTML = [
    ["zetic", "ZETIC Engine"],
    ["llama", "llama.cpp"],
  ]
    .map(
      ([key, label]) =>
        `<div class="chart-row"><div class="chart-label"><span>${label}</span><span>${a[key].toFixed(a.unit === "ms" ? 0 : 1)} ${a.unit}</span></div><div class="bar-track"><div class="bar ${key}" style="width:${(a[key] / max) * 100}%"></div></div></div>`,
    )
    .join("");
  $("chart-source").textContent = isLive
    ? "This comparison · view conditions ↗"
    : "Recorded reference · view conditions ↗";
  $("source-details").innerHTML =
    `<div class="source-content"><strong>${escapeHtml(a.label)} · ${escapeHtml(a.metric)}</strong><p>${escapeHtml(a.conditions)}</p><p>${isLive ? "Same conversation snapshot was run sequentially on ZETIC and llama.cpp. Device contention is avoided by running one NPU engine at a time. Each backend returns its native probabilities and timings." : escapeHtml(info.reference.note)}</p>${isLive ? "" : `<p>Source: ${escapeHtml(info.reference.source)}</p>`}<p>Numbers describe performance on this input. Generated wording and probabilities can differ across quantization and runtime implementations.</p></div>`;
}
function inputCount() {
  const n = $("prompt").value.length;
  $("input-count").textContent = n > 300 ? `${n.toLocaleString()} chars` : "";
  $("prompt").style.height = "auto";
  $("prompt").style.height = Math.min(160, $("prompt").scrollHeight) + "px";
}
$("send").onclick = send;
$("prompt").oninput = inputCount;
$("prompt").onkeydown = (e) => {
  if (e.key === "Enter" && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    send();
  }
};
$("stop").onclick = async () => {
  stopAudio();
  status("Stopping…");
  try {
    await api("/api/cancel", {});
  } catch (e) {
    toast(e.message);
  }
};
$("new-chat").onclick = newSession;
$("model").onchange = () => {
  if (!busy) {
    questions = [];
    updateModel();
    save();
  }
};
$("engine-picker")
  .querySelectorAll("button")
  .forEach(
    (b) =>
      (b.onclick = () => {
        engine = b.dataset.engine;
        updateModel();
      }),
  );
$("compare").onchange = () =>
  status(
    $("compare").checked
      ? "Compare mode · identical input, sequential execution"
      : "Chat mode · " + (engine === "zetic" ? "ZETIC" : "llama.cpp"),
  );
$("add-question").onclick = () => {
  questions.push(defaultQuestion());
  renderQuestions();
};
$("attach").onclick = () => $("file-input").click();
$("samples-open").onclick = () => $("samples-dialog").showModal();
$("file-input").onchange = async (e) => {
  for (const file of e.target.files) {
    try {
      status("Preparing " + file.name + "…");
      const data = new FormData();
      data.append("file", file);
      const r = await fetch("/api/upload", { method: "POST", body: data });
      const a = await r.json();
      if (!r.ok) throw Error(a.error);
      if (a.type === "text")
        $("prompt").value += ($("prompt").value ? "\n\n" : "") + a.text;
      else attachments.push(a);
      renderAttachments();
      inputCount();
      status("Attachment ready");
    } catch (err) {
      toast(err.message);
      status(err.message, true);
    }
  }
  e.target.value = "";
};
$("prepare").onclick = async () => {
  if (busy) return;
  setBusy(true);
  status("Preparing " + model().name + "…");
  try {
    const q = requestBody();
    q.engines = [engine];
    q.messages = [{ role: "user", text: "Prepare" }];
    const result = await api("/api/prepare", q);
    status(`${model().name} ready · load ${result.load_ms.toFixed(0)} ms`);
  } catch (e) {
    toast(e.message);
    status(e.message, true);
  } finally {
    setBusy(false);
  }
};
$("settings-open").onclick = () => {
  const form = $("settings-form");
  for (const [key, value] of Object.entries(settings)) {
    const el = form.elements.namedItem(key);
    if (!el) continue;
    if (el.type === "checkbox") el.checked = value;
    else el.value = value;
  }
  $("settings").showModal();
};
$("settings-form").onsubmit = (e) => {
  e.preventDefault();
  const form = e.currentTarget;
  if (!form.reportValidity()) return;
  const next = {};
  for (const [key, value] of Object.entries(defaults)) {
    const el = form.elements.namedItem(key);
    next[key] =
      typeof value === "boolean"
        ? el.checked
        : typeof value === "number"
          ? Number(el.value)
          : el.value;
  }
  if (next.max_tokens >= next.context) {
    toast("Output tokens must be smaller than context length.");
    return;
  }
  settings = next;
  localStorage.setItem("lfm3.settings", JSON.stringify(settings));
  updateModel();
  $("settings").close();
};
$("unload").onclick = async () => {
  try {
    stopAudio();
    await api("/api/unload", {});
    status("Model unloaded · IQ9 released");
    $("settings").close();
  } catch (e) {
    toast(e.message);
  }
};
document
  .querySelectorAll("[data-close]")
  .forEach((b) => (b.onclick = () => b.closest("dialog").close()));
$("chart-select").onchange = () => renderChart();
$("chart-source").onclick = () => $("source-dialog").showModal();
$("export-chat").onclick = () => {
  if (!session?.turns.length) {
    toast("Start a conversation first.");
    return;
  }
  const blob = new Blob(
      [JSON.stringify({ version: 1, session, settings }, null, 2)],
      { type: "application/json" },
    ),
    a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "lfm3-conversation.json";
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
};
$("import-chat").onclick = () => $("import-input").click();
$("import-input").onchange = async (e) => {
  if (busy) return;
  try {
    const data = JSON.parse(await e.target.files[0].text());
    if (!Array.isArray(data.session?.turns))
      throw Error("Invalid conversation file");
    session = data.session;
    session.id = uid();
    if (info.models.some((m) => m.id === session.model))
      $("model").value = session.model;
    questions = session.questions || [];
    updateModel();
    save();
    renderMessages();
    restoreComparison();
    toast(
      "Conversation imported. Media must still exist in this app’s data directory.",
    );
  } catch (err) {
    toast(err.message);
  }
  e.target.value = "";
};
window.addEventListener("pagehide", () => {
  stopAudio();
  if (busy)
    navigator.sendBeacon(
      "/api/cancel",
      new Blob(["{}"], { type: "application/json" }),
    );
});
(async () => {
  try {
    info = await api("/api/info");
    $("connection").textContent = info.device_error
      ? "NPU recovery needed"
      : "On device · ready";
    $("model").innerHTML = info.models
      .map((m) => `<option value="${m.id}">${escapeHtml(m.name)}</option>`)
      .join("");
    $("model").value = info.default_model;
    $("paths").innerHTML =
      info.models
        .map(
          (m) =>
            `<p><strong>${escapeHtml(m.name)}</strong><br>ZETIC: ${escapeHtml(m.sdk_path)} ${m.available.zetic ? "✓" : "(missing)"}<br>llama.cpp: ${escapeHtml(m.gguf_path)} ${m.available.llama ? "✓" : "(missing)"}</p>`,
        )
        .join("") +
      `<p>Logs and media: ${escapeHtml(info.state_dir)}<br>llama.cpp: HTP 2-NSP; speech projector on CPU.<br>ZETIC Score: median level. llama.cpp Score: expected level index.<br>Restart after editing paths in iq9/gui/config.json.</p>`;
    newSession();
    updateModel();
    renderChart();
  } catch (e) {
    $("connection").textContent = "Connection unavailable";
    status(e.message, true);
    toast(e.message);
  }
})();
