// Sample data is optional. The desktop installer asks once; the host tells the page through
// window.__RIGMIND_CONFIG__ (set from <exe>\demo-data.flag). Anything that is not the desktop host (browser preview,
// Lovable, unit tests) keeps the sample data so the design can still be reviewed there.

type Config = { demoData?: boolean };

function config(): Config | undefined {
  return (globalThis as unknown as { __RIGMIND_CONFIG__?: Config }).__RIGMIND_CONFIG__;
}

/** True when sample (demonstration) data should be shown. False in the desktop app unless the installer option was ticked. */
export function demoDataEnabled(): boolean {
  const c = config();
  return c === undefined ? true : c.demoData === true;
}

/** Sample value when demo data is on, the real/empty value otherwise. */
export function demoOr<T>(demo: T, real: T): T {
  return demoDataEnabled() ? demo : real;
}
