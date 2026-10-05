import https from 'node:https';
import os from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { readFile, writeFile, chmod } from 'node:fs/promises';
import { randomBytes, X509Certificate } from 'node:crypto';
import { atomicJSON, privateDirectory, readJSON } from './storage.mjs';
import { privateIPv4, epoch } from './protocol.mjs';

const execute = promisify(execFile);
export function availableInterfaces() {
  return Object.entries(os.networkInterfaces()).flatMap(([name, entries]) => (entries ?? []).filter(entry => entry.family === 'IPv4' && !entry.internal && privateIPv4(entry.address)).map(entry => ({ name, address: entry.address })));
}
export async function generateCertificate(directory, address) {
  if (!privateIPv4(address)) throw new Error('invalid_interface');
  await privateDirectory(directory);
  const key = path.join(directory, 'server-key.pem'); const cert = path.join(directory, 'server-cert.pem'); const config = path.join(directory, 'certificate.cnf');
  await writeFile(config, `[req]\nprompt=no\ndistinguished_name=subject\nx509_extensions=extensions\n[subject]\nCN=AIQuotaPassport\n[extensions]\nsubjectAltName=IP:${address}\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n`, { mode: 0o600 });
  await execute('openssl', ['ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', key], { timeout: 10000 });
  await chmod(key, 0o600);
  await execute('openssl', ['req', '-new', '-x509', '-sha256', '-key', key, '-out', cert, '-days', '365', '-config', config], { timeout: 10000 });
  await chmod(cert, 0o600);
  const certificate = await readFile(cert, 'utf8');
  if (Buffer.byteLength(certificate) > 1536) throw new Error('certificate_bounds');
  return { key, cert };
}
export class PairingService {
  constructor(directory, handler, { dataPort = 4318, interfaces = availableInterfaces } = {}) {
    this.directory = path.join(directory, 'pairing'); this.handler = handler; this.dataPort = dataPort; this.interfaces = interfaces;
    this.server = null; this.config = null; this.lastSeen = null; this.lifecycle = Promise.resolve();
  }
  serial(operation) {
    const result = this.lifecycle.catch(() => {}).then(operation);
    this.lifecycle = result; return result;
  }
  restore() { return this.serial(async () => {
    await privateDirectory(this.directory);
    const saved = await readJSON(path.join(this.directory, 'pairing.json'));
    if (!saved || saved.v !== 1 || !privateIPv4(saved.address) || !/^[A-Za-z0-9_-]{43}$/.test(saved.token ?? '') || saved.key !== path.join(this.directory, 'server-key.pem') || saved.cert !== path.join(this.directory, 'server-cert.pem')) return;
    this.config = saved;
    if (saved.enabled && this.interfaces().some(item => item.address === saved.address)) {
      try { if (!await this.usableCertificate()) throw new Error('expired_certificate'); await this.listen(); }
      catch { this.config.enabled = false; }
    } else this.config.enabled = false;
  }); }
  async usableCertificate() {
    try {
      const cert = new X509Certificate(await readFile(this.config.cert));
      return cert.checkIP(this.config.address) === this.config.address && Date.parse(cert.validTo) > Date.now() && Date.parse(cert.validFrom) <= Date.now();
    } catch { return false; }
  }
  async listen() {
    const { key, cert, address } = this.config;
    const options = { key: await readFile(key), cert: await readFile(cert), minVersion: 'TLSv1.2' };
    const server = https.createServer(options, this.handler);
    server.requestTimeout = 15000; server.headersTimeout = 10000; server.maxConnections = 4;
    try {
      await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(this.dataPort, address, () => { server.removeListener('error', reject); server.on('error', () => {}); resolve(); });
      });
    } catch (error) { server.close(); throw error; }
    this.server = server; this.config.enabled = true;
  }
  start(address) { return this.serial(async () => {
    if (!privateIPv4(address) || !this.interfaces().some(item => item.address === address)) throw new Error('invalid_interface');
    if (this.server?.listening && this.config.address !== address) throw new Error('pairing_address_active');
    const startedListener = !this.server?.listening;
    if (this.config?.address !== address || !await this.usableCertificate()) {
      await this.closeServer();
      const certificate = await generateCertificate(this.directory, address);
      this.config = { v: 1, address, enabled: false, token: randomBytes(32).toString('base64url'), ...certificate };
    }
    if (!this.server?.listening) await this.listen();
    await atomicJSON(path.join(this.directory, 'pairing.json'), this.config);
    this.attempt = { id: randomBytes(16).toString('hex'), startedListener };
    return { base_url: `https://${address}:${this.dataPort}`, pair_token: this.config.token, server_cert_pem: await readFile(this.config.cert, 'utf8'), server_time: epoch(), pairing_session: this.attempt.id };
  }); }
  publicState() { return { enabled: this.server?.listening === true, address: this.config?.address ?? null, base_url: this.config ? `https://${this.config.address}:${this.dataPort}` : null, last_seen: this.lastSeen }; }
  async revoke() {
    await this.closeServer();
    if (this.config) { this.config.enabled = false; this.config.token = randomBytes(32).toString('base64url'); await atomicJSON(path.join(this.directory, 'pairing.json'), this.config); }
    this.attempt = null;
  }
  stop() { return this.serial(() => this.revoke()); }
  abort(id) { return this.serial(async () => {
    if (this.attempt && this.attempt.id === id && this.attempt.startedListener) await this.revoke();
  }); }
  async closeServer() {
    const server = this.server; this.server = null;
    if (server?.listening) { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
  }
  close() { return this.serial(() => this.closeServer()); }
}
