// The hotspot transport of the setup page: the Passport serves this page itself, and the page
// talks to the same origin. The access code travels in a header and lives in page memory only.

export class HttpTransportError extends Error {
  // `device` carries the protocol and firmware a rejecting Passport reported, if it did.
  constructor(code, device = null) {
    super(code);
    this.name = 'HttpTransportError';
    this.code = code;
    this.device = device;
  }
}

export function createHttpTransport(fetchFn) {
  let code = '';
  async function request(path, body) {
    let response;
    try {
      response = await fetchFn(path, {
        method: body ? 'POST' : 'GET',
        cache: 'no-store',
        credentials: 'omit',
        headers: {
          'X-AIQ-Access': code,
          ...(body ? { 'Content-Type': 'application/json' } : {}),
        },
        ...(body ? { body: JSON.stringify(body) } : {}),
      });
    } catch {
      throw new HttpTransportError('http_unreachable');
    }
    let data = null;
    try {
      data = await response.json();
    } catch {
      /* the body is not JSON */
    }
    if (!response.ok || data?.ok === false) {
      const rejection = data?.error_code ?? data?.error ?? 'request_failed';
      throw new HttpTransportError(
        `device_${rejection}`,
        data ? { protocol: data.protocol, firmware: data.firmware } : null,
      );
    }
    if (!data || typeof data !== 'object') throw new HttpTransportError('http_bad_response');
    return data;
  }
  return {
    kind: 'http',
    supported: true,
    setCode(value) {
      code = value;
    },
    hasSession: () => code !== '',
    openPort: async () => {},
    openSession: () => request('/api/state'),
    stateGet: () => request('/api/state'),
    command: (body) => request('/api/command', body),
    jobProvesAdmission: () => false,
    sessionLost() {
      code = '';
    },
    async close() {
      code = '';
    },
  };
}
