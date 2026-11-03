// Browser-only checks against a running app; does not start inference.
const assert = require("node:assert/strict");
const { chromium } = require("playwright");
(async () => {
  const browser = await chromium.launch({
    headless: true,
    args: ["--no-sandbox"],
  });
  const page = await browser.newPage({
    viewport: { width: 1440, height: 1000 },
  });
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto(process.env.GUI_URL || "http://127.0.0.1:8765");
  await page.waitForSelector(".sample-card");
  await page.click("[data-sample=cats]");
  await page.waitForSelector(".attachment-chip img");
  await page.click("#samples-open");
  await page.click("#sample-dialog-grid [data-sample=d1image]");
  await page.waitForSelector(".decision-panel:not([hidden])");
  assert.equal(await page.locator(".option-editor").count(), 4);
  await page.click("[data-option-add]");
  assert.equal(await page.locator(".option-editor").count(), 5);
  await page.click("#add-question");
  assert.equal(await page.locator(".question-editor").count(), 2);
  await page.click("#settings-open");
  await page.locator("[name=context]").fill("4096");
  await page.click(".primary-button");
  assert((await page.locator("#model-caption").innerText()).includes("4,096"));
  await page.setViewportSize({ width: 1024, height: 768 });
  assert.equal(
    await page.evaluate(
      () => document.documentElement.scrollWidth > innerWidth,
    ),
    false,
  );
  const bounds = await page.locator("#send").boundingBox();
  assert(
    bounds.y + bounds.height < 768,
    "Send control must stay inside the viewport",
  );
  assert.deepEqual(errors, []);
  console.log(
    "PASS samples, D1 question editing, settings and bottom controls",
  );
  await browser.close();
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
