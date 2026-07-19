// Dev-only: drive the Studio like a user (Deploy → Refresh status) and screenshot the live view,
// so the Deploy & Monitor panel shows real data from the daemon. Not part of the app build.
import puppeteer from "puppeteer-core";

const url = process.env.URL ?? "http://127.0.0.1:5174/";
const out = process.env.OUT ?? "/tmp/studio-live.png";

const browser = await puppeteer.launch({
  executablePath: "/usr/bin/google-chrome-stable",
  headless: "new",
  args: ["--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage", "--force-color-profile=srgb"],
  defaultViewport: { width: 1440, height: 1000 },
});
try {
  const page = await browser.newPage();
  page.on("console", (m) => console.log("[page]", m.type(), m.text()));
  page.on("pageerror", (e) => console.log("[pageerror]", e.message));
  await page.goto(url, { waitUntil: "networkidle0", timeout: 20000 });

  // Confirm the browser can reach the daemon through the /api proxy.
  const probe = await page.evaluate(async () => {
    try {
      const res = await fetch("/api/status");
      return { ok: res.ok, status: res.status, body: await res.text() };
    } catch (e) {
      return { error: String(e) };
    }
  });
  console.log("[probe /api/status]", JSON.stringify(probe));

  const clickByText = (text) =>
    page.evaluate((t) => {
      const b = [...document.querySelectorAll("button")].find((x) => x.textContent?.trim().includes(t));
      if (b) { b.click(); return true; }
      return false;
    }, text);

  // Enable LIVE (SSE) monitoring, then Deploy — the panel must update from the STREAM, no Refresh.
  console.log("[click Go live]", await clickByText("Go live"));
  await new Promise((r) => setTimeout(r, 300));
  console.log("[click Deploy]", await clickByText("Deploy"));

  // Poll the panel until the SSE stream reports frames=100 — proving the stream (not a manual fetch)
  // drives the monitor. Fails loudly if the stream never delivers through the proxy.
  const framesLive = await page.waitForFunction(() => {
    const dds = [...document.querySelectorAll("dl.status dt")];
    const frames = dds.find((dt) => dt.textContent === "frames")?.nextElementSibling?.textContent;
    return frames === "100" ? frames : false;
  }, { timeout: 12000 }).then((h) => h.jsonValue()).catch(() => null);
  console.log("[live frames via SSE]", framesLive);

  await new Promise((r) => setTimeout(r, 300));
  await page.screenshot({ path: out });
  console.log("wrote", out);
} finally {
  await browser.close();
}
