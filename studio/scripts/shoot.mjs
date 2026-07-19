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
      const b = [...document.querySelectorAll("button")].find((x) => x.textContent?.trim() === t);
      if (b) { b.click(); return true; }
      return false;
    }, text);

  console.log("[click Deploy]", await clickByText("Deploy"));
  await new Promise((r) => setTimeout(r, 800));
  console.log("[click Refresh status]", await clickByText("Refresh status"));

  const gotStatus = await page.waitForSelector("dl.status", { timeout: 8000 }).then(() => true).catch(() => false);
  console.log("[dl.status rendered]", gotStatus);
  await new Promise((r) => setTimeout(r, 400));

  await page.screenshot({ path: out });
  console.log("wrote", out);
} finally {
  await browser.close();
}
