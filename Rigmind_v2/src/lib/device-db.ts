import wasm from "@/assets/sql-wasm.wasm.asset.json";
import type { Database } from "sql.js";
import { isNative, nativeCall } from "@/lib/native-bridge";
let pending: Promise<Database> | undefined;
function openStore(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open("rigmind-sqlite", 1);
    req.onupgradeneeded = () => req.result.createObjectStore("files");
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
async function sqlite() {
  if (!pending)
    pending = (async () => {
      const init = (await import("sql.js")).default;
      const SQL = await init({ locateFile: () => wasm.url });
      const store = await openStore();
      const bytes = await new Promise<Uint8Array | undefined>((resolve, reject) => {
        const r = store.transaction("files").objectStore("files").get("rigmind.db");
        r.onsuccess = () => resolve(r.result);
        r.onerror = () => reject(r.error);
      });
      store.close();
      const db = bytes ? new SQL.Database(bytes) : new SQL.Database();
      db.run("CREATE TABLE IF NOT EXISTS app_records (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
      return db;
    })();
  return pending;
}
export async function readDevice<T>(key: string): Promise<T | undefined> {
  if (isNative()) {
    const value = await nativeCall<T | null>("records.get", { key });
    return value ?? undefined;
  }
  const db = await sqlite();
  const stmt = db.prepare("SELECT value FROM app_records WHERE key=?");
  try {
    stmt.bind([key]);
    return stmt.step() ? (JSON.parse(String(stmt.get()[0])) as T) : undefined;
  } finally {
    stmt.free();
  }
}
export async function writeDevice(key: string, value: unknown) {
  if (isNative()) {
    await nativeCall("records.set", { key, value });
    return;
  }
  const db = await sqlite();
  db.run("INSERT OR REPLACE INTO app_records (key,value) VALUES (?,?)", [
    key,
    JSON.stringify(value),
  ]);
  const bytes = db.export();
  const store = await openStore();
  await new Promise<void>((resolve, reject) => {
    const tx = store.transaction("files", "readwrite");
    tx.objectStore("files").put(bytes, "rigmind.db");
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
  store.close();
}
export async function listDevice<T>(prefix: string): Promise<{ key: string; value: T }[]> {
  if (isNative()) return nativeCall<{ key: string; value: T }[]>("records.list", { prefix });
  const db = await sqlite();
  const stmt = db.prepare("SELECT key, value FROM app_records WHERE key LIKE ? ORDER BY key");
  const out: { key: string; value: T }[] = [];
  try {
    stmt.bind([`${prefix}%`]);
    while (stmt.step()) {
      const [k, v] = stmt.get();
      out.push({ key: String(k), value: JSON.parse(String(v)) as T });
    }
  } finally {
    stmt.free();
  }
  return out;
}
