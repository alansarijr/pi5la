"use strict";

const log = document.getElementById("log");
const stateEl = document.getElementById("job-state");
const startCapture = document.getElementById("start-capture");
const startLive = document.getElementById("start-live");
const stopBtn = document.getElementById("stop-job");
let events = null;

const PRESETS = {
    uart: { rate: "2M", channels: "8", duration: "", silence_timeout: "1s", dma_buf: "" },
    i2c: { rate: "10M", channels: "8", duration: "", silence_timeout: "1s", dma_buf: "" },
    spi: { rate: "50M", channels: "8", duration: "5s", silence_timeout: "500ms", dma_buf: "" },
};

function captureMode() {
    const picked = document.querySelector('input[name="mode"]:checked');
    return picked ? picked.value : "continuous";
}

function formValues(form) {
    const out = {};
    for (const el of form.elements) {
        if (el.name && !el.closest(".hidden")) out[el.name] = el.value;
    }
    return out;
}

function write(text, cls) {
    const span = document.createElement("span");
    span.className = cls || "";
    span.textContent = text + "\n";
    log.appendChild(span);
    log.scrollTop = log.scrollHeight;
}

function setState(job) {
    if (!job) {
        stateEl.textContent = "idle";
        stateEl.className = "state";
        stopBtn.disabled = true;
        startCapture.disabled = startLive.disabled = false;
        return;
    }
    const running = job.state === "running";
    stateEl.textContent = running
        ? `${job.kind} running, ${job.elapsed.toFixed(1)}s`
        : `${job.kind} ${job.state} (exit ${job.exit_code}) after ${job.elapsed.toFixed(1)}s`;
    stateEl.className = "state " + job.state;
    stopBtn.disabled = !running;
    startCapture.disabled = startLive.disabled = running;
    if (!running) loadCaptures();
}

function classFor(event) {
    if (event.type === "decoded") return "decoded";
    if (event.type === "status") return "status";
    return /error|cannot|invalid|DROPPED|overflow/i.test(event.text) ? "err" : "";
}

function follow() {
    if (events) events.close();
    events = new EventSource("/api/events");
    events.onmessage = (msg) => {
        const event = JSON.parse(msg.data);
        write(event.text, classFor(event));
        if (event.type === "status" && event.text.startsWith("exit code")) {
            events.close();
            events = null;
            refreshStatus();
        }
    };
    events.onerror = () => { /* server closed the stream; status polling takes over */ };
}

async function refreshStatus() {
    const res = await fetch("/api/status");
    const data = await res.json();
    setState(data.job);
    return data.job;
}

async function post(url, body) {
    const res = await fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body || {}),
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || `request failed (${res.status})`);
    return data;
}

async function startJob(url, form, extra) {
    log.textContent = "";
    try {
        const data = await post(url, Object.assign(formValues(form), extra || {}));
        (data.warnings || []).forEach((w) => write("warning: " + w, "warn"));
        setState(data.job);
        follow();
    } catch (e) {
        write(e.message, "err");
        refreshStatus();
    }
}

startCapture.addEventListener("click", () =>
    startJob("/api/capture", document.getElementById("capture-form"),
             { monitor: captureMode() === "monitor" }));

/* Monitor mode has its own burst settings. */
document.querySelectorAll('input[name="mode"]').forEach((radio) => {
    radio.addEventListener("change", () => {
        const monitor = captureMode() === "monitor";
        document.querySelectorAll(".m-monitor").forEach((el) =>
            el.classList.toggle("hidden", !monitor));
        startCapture.textContent = monitor ? "Start monitoring" : "Start capture";
    });
});

startLive.addEventListener("click", () =>
    startJob("/api/live", document.getElementById("live-form")));

stopBtn.addEventListener("click", async () => {
    stopBtn.disabled = true;
    await post("/api/stop");
});

document.querySelectorAll("[data-preset]").forEach((btn) => {
    btn.addEventListener("click", () => {
        const preset = PRESETS[btn.dataset.preset];
        const form = document.getElementById("capture-form");
        for (const [key, value] of Object.entries(preset)) {
            if (form.elements[key]) form.elements[key].value = value;
        }
    });
});

/* Protocol pickers show only the fields that protocol needs. */
document.querySelectorAll("select.protocol").forEach((select) => {
    const update = () => {
        const form = select.closest("form");
        for (const p of ["uart", "i2c", "spi"]) {
            form.querySelectorAll(".p-" + p).forEach((el) =>
                el.classList.toggle("hidden", p !== select.value));
        }
    };
    select.addEventListener("change", update);
    update();
});

/* --- captures ------------------------------------------------------- */

const tbody = document.querySelector("#captures tbody");
const decodeBox = document.getElementById("decode-box");
const decodeName = document.getElementById("decode-name");
const decodeOut = document.getElementById("decode-out");

function humanSize(bytes) {
    if (bytes >= 1e9) return (bytes / 1e9).toFixed(2) + " GB";
    if (bytes >= 1e6) return (bytes / 1e6).toFixed(1) + " MB";
    if (bytes >= 1e3) return (bytes / 1e3).toFixed(1) + " kB";
    return bytes + " B";
}

function captureRow(name, label, size, when, extraClass) {
    const tr = document.createElement("tr");
    if (extraClass) tr.className = extraClass;

    const nameCell = document.createElement("td");
    nameCell.textContent = label;
    tr.appendChild(nameCell);

    const sizeCell = document.createElement("td");
    sizeCell.textContent = size;
    tr.appendChild(sizeCell);

    const whenCell = document.createElement("td");
    whenCell.textContent = when;
    tr.appendChild(whenCell);

    const actions = document.createElement("td");
    actions.className = "actions-cell";

    const dl = document.createElement("a");
    dl.href = "/api/captures/" + name.split("/").map(encodeURIComponent).join("/");
    dl.textContent = "Download";
    actions.appendChild(dl);

    const decode = document.createElement("button");
    decode.textContent = "Decode";
    decode.addEventListener("click", () => {
        decodeBox.classList.remove("hidden");
        decodeName.textContent = name;
        decodeOut.textContent = "";
        decodeBox.scrollIntoView({ behavior: "smooth", block: "nearest" });
    });
    actions.appendChild(decode);

    const del = document.createElement("button");
    del.textContent = "Delete";
    del.addEventListener("click", async () => {
        if (!confirm("Delete " + name + "?")) return;
        await fetch("/api/captures/" + name.split("/").map(encodeURIComponent).join("/"),
                    { method: "DELETE" });
        loadCaptures();
    });
    actions.appendChild(del);

    tr.appendChild(actions);
    return tr;
}

async function loadCaptures() {
    const res = await fetch("/api/captures");
    const data = await res.json();
    tbody.textContent = "";
    for (const item of data.captures) {
        if (item.kind === "monitor") {
            const head = document.createElement("tr");
            head.className = "session";

            const title = document.createElement("td");
            title.textContent = `${item.name} — ${item.bursts.length} burst(s)`;
            head.appendChild(title);

            const size = document.createElement("td");
            size.textContent = humanSize(item.size);
            head.appendChild(size);

            const when = document.createElement("td");
            when.textContent = new Date(item.mtime * 1000).toLocaleString();
            head.appendChild(when);

            const actions = document.createElement("td");
            actions.className = "actions-cell";
            const del = document.createElement("button");
            del.textContent = "Delete session";
            del.addEventListener("click", async () => {
                if (!confirm("Delete the whole session " + item.name + "?")) return;
                await fetch("/api/sessions/" + encodeURIComponent(item.name),
                            { method: "DELETE" });
                loadCaptures();
            });
            actions.appendChild(del);
            head.appendChild(actions);
            tbody.appendChild(head);

            for (const burst of item.bursts) {
                const file = burst.name.split("/")[1];
                const gap = burst.gap_before_s != null
                    ? `, ${burst.gap_before_s.toFixed(1)}s after the previous` : "";
                tbody.appendChild(captureRow(
                    burst.name,
                    `↳ ${file} (${(burst.duration_s || 0).toFixed(3)}s${gap})`,
                    humanSize(burst.size),
                    burst.start_time ? new Date(burst.start_time * 1000).toLocaleTimeString() : "",
                    "burst"));
            }
        } else {
            tbody.appendChild(captureRow(item.name, item.name, humanSize(item.size),
                                         new Date(item.mtime * 1000).toLocaleString()));
        }
    }
}

document.getElementById("refresh-captures").addEventListener("click", loadCaptures);
document.getElementById("close-decode").addEventListener("click", () =>
    decodeBox.classList.add("hidden"));

document.getElementById("run-decode").addEventListener("click", async () => {
    const body = formValues(document.getElementById("decode-form"));
    body.name = decodeName.textContent;
    decodeOut.textContent = "decoding...";
    try {
        const data = await post("/api/decode", body);
        decodeOut.textContent = data.text.trim() || "(no output: check the channel numbers)";
    } catch (e) {
        decodeOut.textContent = e.message;
    }
});

refreshStatus().then((job) => { if (job && job.state === "running") follow(); });
loadCaptures();
