/**
 * ClawdPet Extension for Pi Coding Agent
 *
 * Bridges Pi lifecycle & tool events to ClawdPet daemon (TCP 127.0.0.1:8787)
 * so the M5StickS3 desk pet reacts in real time to Pi sessions, tools, and tokens.
 */

import { spawn } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";

const HOST = "127.0.0.1";
const PORT = 8787;
const DAEMON_CANDIDATES = [
	"E:/GithubProject/ClawdPet/ClawdPet/tools/clawdpet_daemon.py",
	path.join(process.env.USERPROFILE || process.env.HOME || "", ".claude", "clawdpet", "clawdpet_daemon.py"),
];

const HEX_RE = /^[0-9a-fA-F]{6}$/;

function readPeacockColor(cwd: string): string {
	if (!cwd) return "";
	try {
		const cfgPath = path.join(cwd, ".vscode", "settings.json");
		if (!fs.existsSync(cfgPath)) return "";
		const cfg = JSON.parse(fs.readFileSync(cfgPath, "utf8"));
		const raw = String(cfg["peacock.color"] || "").replace(/^#/, "").trim();
		return HEX_RE.test(raw) ? raw.toLowerCase() : "";
	} catch {
		return "";
	}
}

function sendOnce(payload: Record<string, unknown>, timeoutMs = 450): Promise<boolean> {
	return new Promise((resolve) => {
		let done = false;
		const finish = (ok: boolean) => {
			if (done) return;
			done = true;
			socket.destroy();
			resolve(ok);
		};
		const socket = net.createConnection({ host: HOST, port: PORT }, () => {
			socket.write(JSON.stringify(payload));
		});
		socket.setTimeout(timeoutMs, () => finish(false));
		socket.on("data", () => finish(true));
		socket.on("error", () => finish(false));
		socket.on("close", () => finish(false));
	});
}

let spawningDaemon = false;

function spawnDaemonIfNeeded(): void {
	if (spawningDaemon) return;
	const script = DAEMON_CANDIDATES.find((p) => p && fs.existsSync(p));
	if (!script) return;
	spawningDaemon = true;
	try {
		const child = spawn("python", [script], {
			detached: true,
			stdio: "ignore",
			windowsHide: true,
		});
		child.unref();
	} catch {
		// ignore
	} finally {
		setTimeout(() => {
			spawningDaemon = false;
		}, 3000);
	}
}

async function sendToDaemon(payload: Record<string, unknown>): Promise<void> {
	if (await sendOnce(payload)) return;
	spawnDaemonIfNeeded();
	await new Promise((r) => setTimeout(r, 1200));
	await sendOnce(payload);
}

function shortToolName(toolName: string): string {
	if (!toolName) return "";
	if (toolName.startsWith("mcp__")) {
		const parts = toolName.split("__");
		return `mcp:${parts[parts.length - 1] || ""}`.slice(0, 8);
	}
	return toolName.slice(0, 8);
}

export default function (pi: ExtensionAPI) {
	let sessionId = "";
	let cwd = process.cwd();
	let projectName = path.basename(cwd.replace(/[\\/]+$/, "")) || "pi";
	let color = readPeacockColor(cwd);
	let activeTui = false;

	function syncContext(ctx: any): void {
		try {
			const sid = ctx?.sessionManager?.getSessionId?.();
			if (typeof sid === "string" && sid.length > 0) {
				sessionId = `pi-${sid}`;
			}
		} catch {
			// ignore
		}
		if (!sessionId) {
			sessionId = `pi-${process.pid}`;
		}
		const nextCwd = ctx?.cwd || process.cwd();
		if (nextCwd && nextCwd !== cwd) {
			cwd = nextCwd;
			color = readPeacockColor(cwd);
		}
		const envName = (process.env.CLAWDPET_NAME || "").trim();
		projectName = envName || path.basename(cwd.replace(/[\\/]+$/, "")) || "pi";
	}

	function emitEvent(event: string, tool = ""): void {
		if (!activeTui) return;
		void sendToDaemon({
			event,
			session_id: sessionId || `pi-${process.pid}`,
			cwd,
			name: projectName,
			color,
			agent: "pi",
			tool: shortToolName(tool),
		});
	}

	pi.events.on("herdr:blocked", (data: any) => {
		if (!activeTui) return;
		if (data?.active) {
			emitEvent("NotificationChoosing", "wait");
		} else {
			emitEvent("PostToolUse", "think");
		}
	});

	pi.events.on("clawdpet:choosing", (data: any) => {
		if (!activeTui) return;
		if (data?.active) {
			emitEvent("NotificationChoosing", "wait");
		} else {
			emitEvent("PostToolUse", "think");
		}
	});

	pi.on("session_start", async (_event, ctx: any) => {
		if (ctx?.mode && ctx.mode !== "tui") return;
		activeTui = true;
		syncContext(ctx);
		emitEvent("SessionStart");
		if (ctx?.isIdle?.() === false) {
			emitEvent("UserPromptSubmit", "think");
		}
	});

	pi.on("agent_start", (_event, ctx: any) => {
		if (!activeTui) return;
		syncContext(ctx);
		emitEvent("UserPromptSubmit", "think");
	});

	pi.on("tool_execution_start", (event: any, ctx: any) => {
		if (!activeTui) return;
		syncContext(ctx);
		emitEvent("PreToolUse", String(event?.toolName || "tool"));
	});

	pi.on("tool_execution_end", (_event: any, ctx: any) => {
		if (!activeTui) return;
		syncContext(ctx);
		emitEvent("PostToolUse", "think");
	});

	pi.on("agent_settled", (_event, ctx: any) => {
		if (!activeTui || ctx?.isIdle?.() === false) return;
		syncContext(ctx);
		emitEvent("Stop");
	});

	pi.on("session_shutdown", () => {
		if (!activeTui) return;
		emitEvent("SessionEnd");
	});
}
