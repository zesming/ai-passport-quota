import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { DeviceSerialError, serialErrorMessage, startDeviceSerial } from './serial.mjs';
import { deepSeekRmbDisplay } from './deepseek-display.mjs';
import { INTERVALS as REFRESH_OPTIONS, SCREEN_TIMEOUT_SECONDS as SCREEN_TIMEOUT_OPTIONS, DEFAULT_SCREEN_TIMEOUT_SECONDS, MAX_ACCOUNTS, privateIPv4 } from '../shared/contract.mjs';

const PREVIEW_SCREENS = [
  ['home', '主页面'],
  ['settings', '设置'],
  ['accounts', '账户'],
  ['add', '添加账户'],
];

function Logo({ provider, size = 28 }) {
  const openAI = provider === 'codex';
  const deepSeek = provider === 'deepseek';
  return (
    <img
      className={`brand-logo ${openAI ? 'openai' : deepSeek ? 'deepseek' : 'claude'}`}
      src={openAI ? '/assets/openai.svg' : deepSeek ? '/assets/deepseek.svg' : '/assets/claude.svg'}
      alt={openAI ? 'OpenAI' : deepSeek ? 'DeepSeek' : 'Claude'}
      width={size}
      height={size}
    />
  );
}

function providerName(provider, label = '') {
  if (provider === 'codex') return 'Codex';
  if (provider === 'claude') return 'Claude';
  if (provider === 'deepseek') return label || 'DeepSeek API';
  return '账户';
}

function deviceSafeText(value) {
  return Array.from(typeof value === 'string' ? value : '').map(character => /^[\x20-\x7e]$/.test(character) ? character : '?').join('');
}

function DeepSeekBalance({ balance, compact = false }) {
  const { info, availability, tone } = deepSeekRmbDisplay(balance);
  if (compact) {
    return <div className="device-deepseek-balances">
      <section>
        <div className="balance-heading">人民币可用余额</div>
        <div className={`balance-total ${info ? '' : 'unknown'}`} style={info ? { fontSize: info.total_balance.length > 16 ? 12 : info.total_balance.length > 12 ? 14 : 20 } : undefined}>{info?.total_balance || availability}</div>
      </section>
    </div>;
  }
  return (
    <div className="deepseek-balance">
      <div className={`deepseek-availability ${tone}`}>{availability}</div>
      {info && (
        <section className="currency-balance">
          <header><strong>人民币</strong><span>{info.total_balance}</span></header>
        </section>
      )}
    </div>
  );
}

function planLabel(plan) {
  if (typeof plan !== 'string' || !plan.trim()) return '';
  const value = plan.trim();
  return `${value.charAt(0).toLocaleUpperCase()}${value.slice(1)}`;
}

function screenTimeoutLabel(seconds) {
  if (seconds === 0) return '永不';
  if (seconds < 60) return `${seconds} 秒`;
  return `${seconds / 60} 分钟`;
}

function formatLocalClock(nowSeconds) {
  return new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit', hour12: false })
    .format(new Date(nowSeconds * 1000));
}

function DeviceHeader({ title, info, nowSeconds, previewStatus }) {
  const wifiConnected = typeof previewStatus?.wifi_connected === 'boolean' ? previewStatus.wifi_connected : null;
  const batteryPercent = Number.isInteger(previewStatus?.battery_percent)
    && previewStatus.battery_percent >= 0 && previewStatus.battery_percent <= 100
    ? previewStatus.battery_percent : null;
  const wifiLabel = wifiConnected === null ? '设备 Wi‑Fi 状态未知' : wifiConnected ? '设备 Wi‑Fi 已连接' : '设备 Wi‑Fi 未连接';
  const batteryLabel = batteryPercent === null ? '设备电量未知' : `设备电量 ${batteryPercent}%`;
  return (
    <div className="device-header">
      <strong>{title}</strong>
      <span className="device-header-info">{info}</span>
      <span className="device-clock" title="电脑本地时间预览；设备时钟状态未知" aria-label={`电脑本地时间预览 ${formatLocalClock(nowSeconds)}`}>{formatLocalClock(nowSeconds)}</span>
      <span className={`device-wifi ${wifiConnected === null ? 'unknown' : wifiConnected ? 'available' : 'unavailable'}`} title={wifiLabel} aria-label={wifiLabel}>
        <svg viewBox="0 0 18 14" aria-hidden="true" focusable="false">
          <path d="M1.3 4.7a11 11 0 0 1 15.4 0M4.1 7.6a7 7 0 0 1 9.8 0M6.9 10.4a3 3 0 0 1 4.2 0" />
          <circle cx="9" cy="12.3" r=".8" />
          {wifiConnected === false && <path className="wifi-slash" d="M2 1.5 16 13" />}
        </svg>
      </span>
      <span className={`device-battery ${batteryPercent === null ? 'unknown' : 'available'}`} title={batteryLabel} aria-label={batteryLabel}>
        <svg viewBox="0 0 29 12" aria-hidden="true" focusable="false">
          <rect className="battery-outline" x=".5" y=".5" width="25" height="11" rx="2" />
          {batteryPercent !== null && <rect className="battery-fill" x="2" y="2" width={21 * batteryPercent / 100} height="8" rx="1" />}
          {batteryPercent === null && <path className="battery-unknown" d="M10 9 17 3" />}
          <path className="battery-terminal" d="M27 4v4" />
        </svg>
      </span>
    </div>
  );
}

function formatDate(epochSeconds) {
  if (!Number.isFinite(epochSeconds)) return '—';
  return new Intl.DateTimeFormat('zh-CN', {
    month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false,
  }).format(new Date(epochSeconds * 1000));
}

function formatAge(epochSeconds, nowSeconds) {
  if (!Number.isFinite(epochSeconds)) return '尚无采集记录';
  const seconds = Math.max(0, nowSeconds - epochSeconds);
  if (seconds < 60) return '刚刚更新';
  if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前更新`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时前更新`;
  return `${Math.floor(seconds / 86400)} 天前更新`;
}

function accountStatus(account) {
  if (!account) return '无账户';
  if (account.provider === 'deepseek' && account.status === 'expired') return 'API 密钥无效';
  if (account.status === 'expired') return '登录已过期';
  if (account.status === 'error') return '连接错误';
  if (account.status === 'unsupported') return '暂不支持';
  if (account.provider === 'deepseek' && account.status === 'waiting') return '等待余额校验';
  if (!account.authenticated) return '等待登录';
  if (account.status === 'waiting') return account.provider === 'claude' ? '等待首次额度数据' : '等待额度采集';
  return account.status === 'ok' ? '已连接' : account.status;
}

function statusTone(account) {
  if (!account) return 'muted';
  if (account.status === 'expired' || account.status === 'error') return 'warning';
  if (account.status === 'waiting' || !account.authenticated) return 'muted';
  if (account.status === 'ok') return 'good';
  return 'muted';
}

function friendlyError(code) {
  const messages = {
    account_not_found: '此账户已从本机移除。',
    invalid_provider: '所选平台暂不支持。',
    invalid_interface: '所选地址已不可用，请重新选择本机网络。',
    pairing_address_active: '请先停止当前同步，再切换局域网地址。',
    invalid_settings: '设置值无效，请重新选择刷新间隔。',
    invalid_api_key: 'DeepSeek API 密钥格式无效。',
    invalid_account_label: '本机别名不能超过 32 个 UTF-8 字节或包含控制字符。',
    account_busy: '此账户正在更新，请稍后重试。',
    invalid_login_code: '验证码无效或登录流程已结束，请重新开始官方登录。',
    account_limit: `最多可连接 ${MAX_ACCOUNTS} 个账户。`,
    cli_unavailable: '本机未发现该平台的官方命令行工具。',
    unsupported_account: '此账户不支持当前操作。',
    invalid_csrf: '本机安全令牌已失效，请刷新页面后重试。',
    local_service_error: '本机服务未能完成请求，请稍后重试。',
  };
  return messages[code] ?? code ?? '请求未完成，请稍后重试。';
}

function friendlyLoginError(code) {
  if (code === 'cli_unavailable') return '本机未发现所需的官方命令行工具。';
  if (code === 'login_timeout') return '官方登录流程超时，请重新开始并及时完成授权。';
  if (code === 'login_failed') return '官方登录流程未完成，请重新开始登录。';
  return '请重新开始官方登录流程。';
}

function remainingDuration(seconds) {
  const hours = Math.floor(seconds / 3600);
  return hours < 1 ? '<1h' : `${Math.floor(hours / 24)}d ${hours % 24}h`;
}

function quotaView(windowValue, nowSeconds) {
  if (!windowValue || !Number.isInteger(windowValue.remaining_percent)) {
    return { value: null, reset: '', expired: false };
  }
  const resetAt = windowValue.resets_at;
  const expired = Number.isFinite(resetAt) && resetAt <= nowSeconds;
  return {
    value: expired ? null : Math.max(0, Math.min(100, windowValue.remaining_percent)),
    reset: Number.isFinite(resetAt) && !expired ? `🔄 ${remainingDuration(resetAt - nowSeconds)}` : '',
    expired,
  };
}

function QuotaBar({ label, windowValue, nowSeconds, stale = false, compact = false }) {
  const quota = quotaView(windowValue, nowSeconds);
  const tone = quota.value === null ? 'unknown' : quota.value <= 5 ? 'critical' : quota.value <= 20 ? 'low' : 'available';
  const resetText = quota.expired ? '等待新数据' : quota.reset || '重置时间未知';
  return (
    <section className={`quota ${tone} ${stale ? 'stale' : ''} ${compact ? 'compact' : ''}`}>
      <div className="quota-label">
        <strong>{label} 剩余</strong>
        <span>{quota.value === null ? (quota.expired ? '等待新数据' : '未提供') : resetText}</span>
      </div>
      <div className="quota-value">{quota.value === null ? compact ? quota.expired ? '等待新数据' : '未提供' : '—' : <>{quota.value}<span>%</span></>}</div>
      <div
        className="quota-track"
        role="progressbar"
        aria-label={`${label} 剩余额度`}
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={quota.value ?? undefined}
        aria-valuetext={quota.value === null ? (quota.expired ? '窗口已重置，等待新数据' : '此账户未提供此窗口') : `${quota.value}% 剩余`}
      >
        <div style={{ width: `${quota.value ?? 0}%` }} />
      </div>
    </section>
  );
}

function visibleWindows(account) {
  return [['five_hour', '5h'], ['seven_day', '7 天']]
    .filter(([key]) => account[key] != null);
}

function SubscriptionQuota({ account, nowSeconds, stale, compact = false }) {
  const windows = visibleWindows(account);
  const extras = [];
  if (account.provider === 'codex') {
    if (account.banked_reset?.available_count > 0) {
      const expiry = account.banked_reset.next_expires_at;
      extras.push(['可用重置', Number.isFinite(expiry)
        ? `${account.banked_reset.available_count}次 · ${expiry <= nowSeconds ? '等待更新' : `${remainingDuration(expiry - nowSeconds)} 到期`}`
        : `${account.banked_reset.available_count} 次`]);
    }
    if (account.credits?.has_credits || account.credits?.unlimited) {
      extras.push(['剩余额度', account.credits.unlimited ? '不限量' : account.credits.balance || '可用']);
    }
  }
  return <div className={compact ? `device-subscription ${windows.length === 1 ? 'single-window' : ''}` : 'subscription-quota'}>
    <div className={compact ? 'device-windows' : 'summary-bars'}>
      {windows.map(([key, label]) => <QuotaBar key={key} label={label} windowValue={account[key]} nowSeconds={nowSeconds} stale={stale} compact={compact} />)}
      {!windows.length && <div className="no-windows">{account.status === 'ok' ? '未提供额度窗口' : '等待额度数据'}</div>}
    </div>
    {extras.length > 0 && <div className={`quota-extras ${stale ? 'stale' : ''}`} style={compact ? { top: windows.length === 2 ? 119 : windows.length === 1 ? 90 : 42 } : undefined}>
      {extras.map(([label, value]) => <div key={label}><span>{label}</span><strong title={value}>{compact && label === '剩余额度' && !account.credits.unlimited && account.credits.balance ? deviceSafeText(value) : value}</strong></div>)}
    </div>}
  </div>;
}

function byteLength(value) {
  return new TextEncoder().encode(value).length;
}

function makeRequestId() {
  if (!globalThis.crypto?.getRandomValues) throw new Error('此浏览器无法生成安全的设备请求编号。');
  const random = new Uint8Array(4);
  globalThis.crypto.getRandomValues(random);
  return Array.from(random, byte => byte.toString(16).padStart(2, '0')).join('');
}

async function readResponse(response) {
  const text = await response.text();
  let data = {};
  if (text) {
    try { data = JSON.parse(text); } catch { data = {}; }
  }
  if (!response.ok) throw new Error(friendlyError(typeof data.error === 'string' ? data.error : `请求失败（${response.status}）`));
  return data;
}

export function App() {
  const [apiState, setApiState] = useState(null);
  const [apiError, setApiError] = useState('');
  const [selectedId, setSelectedId] = useState(null);
  const [panel, setPanel] = useState('accounts');
  const [previewScreen, setPreviewScreen] = useState('home');
  const [settingFocus, setSettingFocus] = useState(0);
  const [toast, setToast] = useState('');
  const [busyAction, setBusyAction] = useState('');
  const [providerDialog, setProviderDialog] = useState(false);
  const [providerChoice, setProviderChoice] = useState('codex');
  const [newDeepSeekApiKey, setNewDeepSeekApiKey] = useState('');
  const [newDeepSeekLabel, setNewDeepSeekLabel] = useState('DeepSeek API');
  const [accountDeepSeekApiKey, setAccountDeepSeekApiKey] = useState('');
  const [accountDeepSeekLabel, setAccountDeepSeekLabel] = useState('');
  const [loginSessions, setLoginSessions] = useState({});
  const [loginCode, setLoginCode] = useState('');
  const [scale, setScale] = useState(() => window.innerWidth < 440 ? 1 : 1.35);
  const [nowSeconds, setNowSeconds] = useState(() => Math.floor(Date.now() / 1000));
  const [wifiSsid, setWifiSsid] = useState('');
  const [wifiPassword, setWifiPassword] = useState('');
  const [serialError, setSerialError] = useState('');
  const [serialMessage, setSerialMessage] = useState('');
  const [serialBusy, setSerialBusy] = useState(false);
  const [selectedAddress, setSelectedAddress] = useState('');
  const [copiedLaunch, setCopiedLaunch] = useState(false);
  const pollLock = useRef(false);
  const pendingAccountSelection = useRef(null);
  const pressTimer = useRef(null);
  const wasLongPress = useRef(false);
  const currentAccountList = apiState?.accounts ?? [];
  const account = currentAccountList.find(item => item.id === selectedId) ?? currentAccountList[0] ?? null;
  const authenticatedAccounts = currentAccountList.filter(item => item.authenticated === true);
  const refreshableAccounts = currentAccountList.filter(item => item.provider === 'deepseek' ? item.status !== 'expired' : item.authenticated === true);
  const previewAccount = account?.authenticated === true ? account : authenticatedAccounts[0] ?? null;
  const previewAccountIndex = previewAccount ? authenticatedAccounts.findIndex(item => item.id === previewAccount.id) : -1;
  const selectedPendingLogin = account
    ? loginSessions[account.id]
      ?? (apiState?.pending_logins ?? []).find(item => item.account_id === account.id)
      ?? null
    : null;
  const addresses = useMemo(() => {
    const result = [];
    for (const iface of apiState?.interfaces ?? []) {
      if (privateIPv4(iface.address)) result.push({ name: iface.name, address: iface.address });
    }
    return result;
  }, [apiState?.interfaces]);
  const refreshSeconds = apiState?.settings?.refresh_seconds;
  const autoRefresh = apiState?.settings?.auto_refresh;
  const screenTimeoutSeconds = apiState?.settings?.screen_timeout_seconds ?? DEFAULT_SCREEN_TIMEOUT_SECONDS;
  const previewStatus = apiState?.preview_status;
  const stale = Boolean(account && Number.isFinite(account.observed_at)
    && nowSeconds - account.observed_at > Math.max(900, 2 * (refreshSeconds || 0)));
  const previewStale = Boolean(previewAccount && Number.isFinite(previewAccount.observed_at)
    && nowSeconds - previewAccount.observed_at > Math.max(900, 2 * (refreshSeconds || 0)));
  const selectedCached = Boolean(apiError || stale || ['expired', 'error'].includes(account?.status));
  const previewCached = Boolean(apiError || previewStale || ['expired', 'error'].includes(previewAccount?.status));
  const statusText = account
    ? apiError ? `本机连接中断 · 缓存数据 · ${formatAge(account.observed_at, nowSeconds)}`
      : stale ? `数据较旧 · ${formatAge(account.observed_at, nowSeconds)}` : formatAge(account.observed_at, nowSeconds)
    : apiError ? '本机应用不可达' : '尚未连接账户';

  const pollState = useCallback(async () => {
    if (pollLock.current) return;
    pollLock.current = true;
    const controller = new AbortController();
    const timeoutId = window.setTimeout(() => controller.abort(), 7000);
    try {
      const response = await fetch('/api/state', { cache: 'no-store', signal: controller.signal });
      const nextState = await readResponse(response);
      setApiState(nextState);
      setApiError('');
    } catch {
      setApiError('无法连接本机应用。请确认 AI Passport 桌面程序正在运行；已保留最近一次数据。');
    } finally {
      window.clearTimeout(timeoutId);
      pollLock.current = false;
    }
  }, []);

  useEffect(() => {
    pollState();
    const intervalId = window.setInterval(pollState, 5000);
    return () => window.clearInterval(intervalId);
  }, [pollState]);

  useEffect(() => {
    const timer = window.setInterval(() => setNowSeconds(Math.floor(Date.now() / 1000)), 5000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    if (!toast) return undefined;
    const timer = window.setTimeout(() => setToast(''), 3600);
    return () => window.clearTimeout(timer);
  }, [toast]);

  useEffect(() => {
    if (currentAccountList.some(item => item.id === selectedId)) pendingAccountSelection.current = null;
    if (pendingAccountSelection.current === selectedId && selectedId) return;
    if (!selectedId || !currentAccountList.some(item => item.id === selectedId)) {
      setSelectedId(currentAccountList[0]?.id ?? null);
    }
  }, [currentAccountList, selectedId]);

  useEffect(() => {
    setLoginCode('');
  }, [account?.id]);

  useEffect(() => {
    setAccountDeepSeekApiKey('');
    setAccountDeepSeekLabel(account?.provider === 'deepseek' ? account.label || 'DeepSeek API' : '');
  }, [account?.id, account?.label, account?.provider]);

  useEffect(() => {
    if (!providerDialog) setNewDeepSeekApiKey('');
  }, [providerDialog]);

  useEffect(() => {
    if (!apiState?.pending_logins?.length) return;
    setLoginSessions(previous => {
      let changed = false;
      const next = { ...previous };
      for (const job of apiState.pending_logins) {
        if (previous[job.account_id]) {
          next[job.account_id] = job;
          changed = true;
        }
      }
      return changed ? next : previous;
    });
  }, [apiState?.pending_logins]);

  useEffect(() => {
    if (!addresses.some(item => item.address === selectedAddress)) setSelectedAddress(addresses[0]?.address ?? '');
  }, [addresses, selectedAddress]);

  useEffect(() => {
    if (panel !== 'setup') setWifiPassword('');
  }, [panel]);

  useEffect(() => () => window.clearTimeout(pressTimer.current), []);

  useEffect(() => {
    const resize = () => setScale(window.innerWidth < 440 ? Math.max(0.75, Math.min(1, (window.innerWidth - 56) / 240)) : 1.35);
    window.addEventListener('resize', resize);
    return () => window.removeEventListener('resize', resize);
  }, []);

  async function requestJson(path, { method = 'GET', body } = {}) {
    const headers = { Accept: 'application/json' };
    const options = { method, headers, cache: 'no-store' };
    if (method !== 'GET') {
      if (!apiState?.csrf_token) throw new Error('本机安全令牌尚未载入，请稍候重试。');
      headers['Content-Type'] = 'application/json';
      headers['X-AIQ-CSRF'] = apiState.csrf_token;
      options.body = JSON.stringify(body ?? {});
    }
    return readResponse(await fetch(path, options));
  }

  async function mutate(path, { method = 'POST', body, success } = {}) {
    if (busyAction) return null;
    setBusyAction(path);
    try {
      const result = await requestJson(path, { method, body });
      if (success) setToast(success);
      pollState();
      return result;
    } catch (error) {
      setToast(error?.message || '请求未完成，请稍后重试。');
      return null;
    } finally {
      setBusyAction('');
    }
  }

  function moveAccount(delta) {
    if (authenticatedAccounts.length < 2) return;
    const nextIndex = (Math.max(previewAccountIndex, 0) + delta + authenticatedAccounts.length) % authenticatedAccounts.length;
    setSelectedId(authenticatedAccounts[nextIndex].id);
  }

  async function refreshAccount(accountId) {
    const refreshableAccount = currentAccountList.find(item => item.id === accountId && (item.authenticated === true || item.provider === 'deepseek'));
    if (!refreshableAccount) {
      setToast('完成官方登录后，才能刷新此账户的额度。');
      return;
    }
    await mutate(`/api/accounts/${accountId}/refresh`, { success: refreshableAccount.provider === 'deepseek' ? '余额查询已提交。' : '刷新请求已提交。额度会在本机采集完成后更新。' });
  }

  async function refreshAll() {
    if (!refreshableAccounts.length) {
      setToast('当前没有可刷新的账户。');
      return;
    }
    await mutate('/api/refresh', { success: '刷新请求已提交。' });
  }

  async function updateSettings(nextSettings, success) {
    const settings = {
      refresh_seconds: refreshSeconds,
      auto_refresh: Boolean(autoRefresh),
      screen_timeout_seconds: screenTimeoutSeconds,
      ...nextSettings,
    };
    if (refreshSeconds === settings.refresh_seconds && autoRefresh === settings.auto_refresh
      && screenTimeoutSeconds === settings.screen_timeout_seconds) return;
    await mutate('/api/settings', { method: 'PATCH', body: settings, success });
  }

  function openProviderDialog() {
    setProviderChoice('codex');
    setNewDeepSeekApiKey('');
    setNewDeepSeekLabel('DeepSeek API');
    setProviderDialog(true);
  }

  async function startLogin(provider, options = {}) {
    if ((apiState?.accounts?.length ?? 0) >= MAX_ACCOUNTS) {
      setToast(`最多可连接 ${MAX_ACCOUNTS} 个账户。`);
      return;
    }
    const result = await mutate('/api/accounts', {
      body: { provider, ...options },
      success: provider === 'deepseek' ? 'DeepSeek 余额账户已添加。本机只查询余额。' : '已创建登录流程。请在下方打开官方验证页并完成授权。',
    });
    if (result?.account_id) {
      pendingAccountSelection.current = result.account_id;
      setSelectedId(result.account_id);
      if (result.login) setLoginSessions(current => ({ ...current, [result.account_id]: result.login }));
      setProviderDialog(false);
      setPanel('accounts');
      setPreviewScreen('home');
      pollState();
    }
  }

  async function addDeepSeekAccount(event) {
    event.preventDefault();
    const apiKey = newDeepSeekApiKey;
    const label = newDeepSeekLabel;
    setNewDeepSeekApiKey('');
    await startLogin('deepseek', { api_key: apiKey, label });
  }

  async function updateDeepSeekAccount(event, accountId) {
    event.preventDefault();
    const apiKey = accountDeepSeekApiKey;
    const label = accountDeepSeekLabel;
    const current = currentAccountList.find(item => item.id === accountId);
    setAccountDeepSeekApiKey('');
    if (!apiKey && label === (current?.label || 'DeepSeek API')) {
      setToast('输入新的 API 密钥，或修改本机别名后保存。');
      return;
    }
    const body = { label };
    if (apiKey) body.api_key = apiKey;
    await mutate(`/api/accounts/${accountId}/api-key`, {
      body,
      success: apiKey ? '已保存别名并开始验证新 API 密钥。' : '本机别名已更新。',
    });
  }

  async function retryLogin(accountId) {
    const result = await mutate(`/api/accounts/${accountId}/login`, { success: '新的官方登录流程已准备好。' });
    if (result?.login) setLoginSessions(current => ({ ...current, [accountId]: result.login }));
  }

  async function submitLoginCode(accountId) {
    const code = loginCode.trim();
    if (!code) {
      setToast('请输入官方登录流程要求的验证码。');
      return;
    }
    const result = await mutate(`/api/accounts/${accountId}/login-code`, {
      body: { code },
      success: '验证码已提交给本机登录流程。',
    });
    if (result) setLoginCode('');
  }

  async function removeAccount(accountId) {
    const result = await mutate(`/api/accounts/${accountId}`, {
      method: 'DELETE',
      body: {},
      success: '账户已从本机移除。',
    });
    if (result) setLoginSessions(current => {
      const next = { ...current };
      delete next[accountId];
      return next;
    });
  }

  async function copyText(text, onSuccess) {
    try {
      await navigator.clipboard.writeText(text);
      onSuccess?.();
      setToast('已复制到剪贴板。');
    } catch {
      setToast('无法访问剪贴板，请手动复制。');
    }
  }

  async function copyClaudeLaunch(accountId) {
    setCopiedLaunch(false);
    try {
      const result = await requestJson(`/api/accounts/${accountId}/launch`);
      if (typeof result.command !== 'string' || !result.command) throw new Error('本机没有返回启动命令。');
      await copyText(result.command, () => setCopiedLaunch(true));
    } catch (error) {
      setToast(error?.message || '无法获取启动命令。');
    }
  }

  function activateSetting(index) {
    if (index === 0) setPreviewScreen('accounts');
    if (index === 1 || index === 3) setPanel('refresh');
    if (index === 2) refreshAccount(previewAccount?.id);
    if (index === 4) setPanel('setup');
  }

  function shortOK() {
    if (previewScreen === 'home') refreshAccount(previewAccount?.id);
    else if (previewScreen === 'settings') {
      activateSetting(settingFocus);
    } else if (previewScreen === 'accounts') {
      if (account) setPreviewScreen('home');
      else setPreviewScreen('add');
    } else if (previewScreen === 'add') {
      setProviderDialog(true);
    }
  }

  function longOK() {
    setPreviewScreen(current => current === 'home' ? 'settings' : 'home');
    setSettingFocus(0);
  }

  function direction(delta) {
    if (previewScreen === 'home' || previewScreen === 'accounts') moveAccount(delta);
    else if (previewScreen === 'settings') setSettingFocus(current => (current + delta + 5) % 5);
  }

  useEffect(() => {
    function onKeyDown(event) {
      if (event.target instanceof HTMLElement && /INPUT|TEXTAREA|SELECT|BUTTON/.test(event.target.tagName)) return;
      if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
        event.preventDefault();
        direction(event.key === 'ArrowUp' ? -1 : 1);
      } else if (event.key === 'Enter' && !event.repeat) {
        event.preventDefault();
        shortOK();
      } else if (event.key === 'Escape') {
        event.preventDefault();
        longOK();
      }
    }
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  });

  function startOKPress(event) {
    event.currentTarget.setPointerCapture?.(event.pointerId);
    wasLongPress.current = false;
    window.clearTimeout(pressTimer.current);
    pressTimer.current = window.setTimeout(() => {
      wasLongPress.current = true;
      longOK();
    }, 650);
  }

  function endOKPress() {
    window.clearTimeout(pressTimer.current);
    window.setTimeout(() => { wasLongPress.current = false; }, 0);
  }

  async function configureDevice(event) {
    event.preventDefault();
    setSerialError('');
    setSerialMessage('');
    const ssidBytes = byteLength(wifiSsid);
    const passwordBytes = byteLength(wifiPassword);
    if (!wifiSsid.length || ssidBytes > 32) {
      setSerialError('Wi‑Fi 名称必填，且不能超过 32 个 UTF‑8 字节。');
      return;
    }
    if (passwordBytes > 64) {
      setSerialError('Wi‑Fi 密码不能超过 64 个 UTF‑8 字节。');
      return;
    }
    if (!selectedAddress || !addresses.some(item => item.address === selectedAddress)) {
      setSerialError('请先选择一个本机私有 IPv4 地址。');
      return;
    }
    if (!navigator.serial?.requestPort) {
      setSerialError('此浏览器没有 Web Serial 支持。请在桌面版 Chrome 或 Edge 中打开本地应用。');
      return;
    }

    let port = null;
    let opened = false;
    let serial = null;
    let pairingSession = null;
    setSerialBusy(true);
    try {
      port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }] });
      await port.open({ baudRate: 115200, bufferSize: 4096 });
      opened = true;
      const requestId = makeRequestId();
      serial = startDeviceSerial(port, requestId);

      const pairing = await requestJson('/api/pairing', {
        method: 'POST',
        body: { address: selectedAddress },
      });
      pairingSession = pairing.pairing_session;
      if (typeof pairing.base_url !== 'string' || typeof pairing.pair_token !== 'string'
        || !/^[A-Za-z0-9_-]{43}$/.test(pairing.pair_token)
        || typeof pairing.server_cert_pem !== 'string' || byteLength(pairing.server_cert_pem) > 1536) {
        throw new Error('本机配对服务返回的信息不完整，请重新尝试。');
      }
      let baseUrl;
      try { baseUrl = new URL(pairing.base_url); } catch { throw new Error('本机配对服务返回的信息不完整，请重新尝试。'); }
      if (baseUrl.protocol !== 'https:') throw new Error('本机配对服务返回的信息不完整，请重新尝试。');
      const frame = {
        v: 1,
        op: 'configure',
        request_id: requestId,
        ssid: wifiSsid,
        password: wifiPassword,
        base_url: pairing.base_url,
        pair_token: pairing.pair_token,
        server_cert_pem: pairing.server_cert_pem,
        server_time: pairing.server_time,
      };
      const encoded = new TextEncoder().encode(`@AIQ:${JSON.stringify(frame)}\n`);
      if (encoded.byteLength > 4096) throw new Error('配置内容超过设备协议允许的大小。');
      const ack = await serial.send(encoded);
      if (!ack.ok) throw new DeviceSerialError(`device_${ack.error}`, {});
      setSerialMessage('配置已发送。设备将连接本机安全同步服务。');
      setToast('设备配置完成。');
    } catch (error) {
      if (error?.name === 'NotFoundError' || error?.name === 'AbortError') {
        setSerialError('未选择串口设备。');
      } else if (error?.name === 'InvalidStateError') {
        setSerialError('设备串口正在使用中。请关闭其他配对网页或串口工具后重试。');
      } else if (serialErrorMessage(error)) {
        setSerialError(serialErrorMessage(error));
      } else if (error?.message?.startsWith('请先停止当前同步') || error?.message?.startsWith('配置内容') || error?.message?.startsWith('本机配对服务')) {
        setSerialError(error.message);
      } else {
        setSerialError('设备配置没有完成。请检查线缆和设备配对页面后重试。');
      }
      if (pairingSession) {
        void requestJson('/api/pairing/abort', { method: 'POST', body: { session_id: pairingSession } }).catch(() => {});
      }
    } finally {
      try { await serial?.close(); } catch { /* close port below */ }
      if (port && opened) {
        try { await port.close(); } catch { /* device may already have closed */ }
      }
      setWifiPassword('');
      setSerialBusy(false);
    }
  }

  async function stopDeviceSync() {
    const result = await mutate('/api/pairing/stop', { body: {}, success: '已停止本机设备同步服务。' });
    if (result) setSerialMessage('设备同步服务已停止。');
  }

  const connectedStatus = apiError ? '本地应用不可达' : apiState ? '本地应用已连接' : '正在连接本地应用';

  return (
    <main className="app-shell">
      <header className="page-header">
        <div className="wordmark">AI Passport <span>本地额度看板</span></div>
        <div className={`connection-label ${apiError ? 'warning' : apiState ? 'connected' : ''}`}>
          <span className="live-dot" />{connectedStatus}
        </div>
      </header>

      <section className="intro">
        <div>
          <span className="eyebrow">本机账户与设备管理</span>
          <h1>AI 使用额度，一眼看清</h1>
          <p>连接官方账户，查看 AI Passport 小屏额度与本机同步状态。</p>
        </div>
        <div className="local-badge"><span className="lock-mark">⌑</span><span>账户授权保存在此电脑</span><small>本地应用 · 仅本机连接</small></div>
      </section>

      {apiError && <div className="api-banner" role="status"><strong>本机应用暂时无法连接</strong><span>{apiError}</span><button className="secondary" onClick={pollState}>重试连接</button></div>}

      <div className="workspace">
        <section className="preview-panel" aria-labelledby="preview-title">
          <div className="section-heading"><div><h2 id="preview-title">设备屏幕预览</h2><span>显示本机实际账户与额度</span></div><span className="screen-size">240 × 320</span></div>
          <nav className="screen-tabs" aria-label="切换设备预览画面">
            {PREVIEW_SCREENS.map(([value, label]) => (
              <button key={value} type="button" className={previewScreen === value ? 'active' : ''} aria-pressed={previewScreen === value} onClick={() => setPreviewScreen(value)}>{label}</button>
            ))}
          </nav>

          <div className="display-stage">
            <div className="screen-wrapper" style={{ width: 240 * scale, height: 320 * scale }}>
              <div className="device-screen" style={{ transform: `scale(${scale})` }} aria-label="240 乘 320 的 AI Passport 设备屏幕预览">
                {previewScreen === 'home' && <>
                  <DeviceHeader title="AI 额度" info={previewAccount ? `${previewAccountIndex + 1}/${authenticatedAccounts.length}` : '0/0'} nowSeconds={nowSeconds} previewStatus={previewStatus} />
                  {previewAccount ? <>
                    <div className={`identity ${previewAccount.provider === 'deepseek' ? 'deepseek-identity' : ''}`}><Logo provider={previewAccount.provider} size={36} /><div><h3>{previewAccount.provider === 'deepseek' ? 'DeepSeek' : previewAccount.provider === 'codex' ? 'ChatGPT' : 'Claude'}</h3><p>{previewAccount.provider === 'deepseek' ? '开放平台 · API' : `${previewAccount.provider === 'codex' ? 'Codex' : 'Claude Code'} · ${planLabel(previewAccount.plan)}`}</p><div className="email" title={previewAccount.provider === 'deepseek' ? '本机别名' : previewAccount.email}>{previewAccount.provider === 'deepseek' ? deviceSafeText(previewAccount.label || 'DeepSeek API') : deviceSafeText(previewAccount.email)}</div></div></div>
                    {previewAccount.provider === 'deepseek' ? <DeepSeekBalance balance={previewAccount.balance} compact /> : <SubscriptionQuota account={previewAccount} nowSeconds={nowSeconds} stale={previewCached} compact />}
                    <div className={`device-update ${previewAccount.provider === 'deepseek' ? 'deepseek-update' : ''} ${previewCached || previewAccount.status !== 'ok' ? 'warning-text' : ''}`}>{previewCached ? '缓存数据 · ' : ''}{previewAccount.provider === 'deepseek' && deepSeekRmbDisplay(previewAccount.balance).info && previewAccount.status === 'ok' && previewAccount.balance?.is_available === false ? '人民币余额不可用' : accountStatus(previewAccount)} · {formatAge(previewAccount.observed_at, nowSeconds)}{previewAccount.provider === 'deepseek' && Number.isFinite(previewAccount.observed_at) ? ` · ${formatDate(previewAccount.observed_at)}` : ''}</div>
                  </> : <div className="empty-device"><strong>{apiState ? '尚无已授权账户' : '连接本机应用中'}</strong><span>{apiState ? '完成电脑端官方登录并采集到额度后，数据会显示在这里。' : '正在读取本机账户状态。'}</span></div>}
                  <div className="device-footer"><span>↑↓ 切换 · OK 刷新</span><span>长按设置</span></div>
                </>}

                {previewScreen === 'settings' && <>
                  <DeviceHeader title="设置" info="" nowSeconds={nowSeconds} previewStatus={previewStatus} />
                  <div className="device-settings">
                    {[
                      ['账户管理', `${authenticatedAccounts.length} 个`],
                      ['自动刷新', apiState ? autoRefresh ? `${refreshSeconds / 60} 分钟` : '关闭' : '—'],
                      ['立即刷新', busyAction.includes('/refresh') ? '提交中' : ''],
                      ['息屏时间', apiState ? screenTimeoutLabel(screenTimeoutSeconds) : '—'],
                      ['配对', '电脑端'],
                    ].map(([label, value], index) => <button key={label} type="button" className={settingFocus === index ? 'focused' : ''} onClick={() => { setSettingFocus(index); activateSetting(index); }}><span>{label}</span><span>{value}</span></button>)}
                  </div>
                  <div className="device-explanation">电脑完成账户授权<br />额度来自最近一次采集</div>
                  <div className="device-footer"><span>↑↓ 选择 · OK 确认</span><span>长按返回</span></div>
                </>}

                {previewScreen === 'accounts' && <>
                  <DeviceHeader title="账户管理" info={String(authenticatedAccounts.length)} nowSeconds={nowSeconds} previewStatus={previewStatus} />
                  {authenticatedAccounts.length ? <div className="device-accounts">
                    {authenticatedAccounts.map(item => <button key={item.id} type="button" className={previewAccount?.id === item.id ? 'focused' : ''} onClick={() => { setSelectedId(item.id); setPreviewScreen('home'); }}><Logo provider={item.provider} size={22} /><span><strong>{providerName(item.provider, item.label)}</strong><small>{item.provider === 'deepseek' ? 'DeepSeek API' : item.email || accountStatus(item)}</small></span></button>)}
                  </div> : <div className="empty-device compact-empty"><span>尚无已授权账户</span><small>在电脑端完成官方登录</small></div>}
                  <button className="device-add" type="button" onClick={() => setPreviewScreen('add')}>在电脑上添加账户</button>
                  <div className="device-footer"><span>↑↓ 切换 · OK 查看</span><span>长按返回</span></div>
                </>}

                {previewScreen === 'add' && <>
                  <DeviceHeader title="添加账户" info={String(currentAccountList.length)} nowSeconds={nowSeconds} previewStatus={previewStatus} />
                  <div className="pairing-intro">请在电脑设置页<br />添加账户并完成连接</div>
                  <div className="device-providers"><div><Logo provider="codex" size={34} /><span>Codex</span></div><div><Logo provider="claude" size={34} /><span>Claude</span></div><div><Logo provider="deepseek" size={34} /><span>DeepSeek</span></div></div>
                  <div className="pairing-code"><span>配置步骤</span><strong>电脑端</strong><small>此预览不模拟配对倒计时</small></div>
                  <div className="device-footer"><span>请通过 USB 提交配置</span><span>长按返回</span></div>
                </>}
              </div>
            </div>
          </div>

          <div className="hardware-controls" aria-label="设备三键操作模拟">
            <button type="button" onClick={() => direction(-1)} aria-label="上键">↑</button>
            <button
              type="button"
              className="ok-key"
              aria-label="OK 键，短按确认，长按切换设置"
              onPointerDown={startOKPress}
              onPointerUp={endOKPress}
              onPointerLeave={endOKPress}
              onPointerCancel={endOKPress}
              onClick={() => { if (!wasLongPress.current) shortOK(); }}
            >OK</button>
            <button type="button" onClick={() => direction(1)} aria-label="下键">↓</button>
          </div>
          <p className="control-hint">也可用键盘 ↑ / ↓、Enter 短按、Esc 长按。屏幕会跟随所选账户更新。</p>
          <div className="device-data-note">缺失的额度窗口不显示；过期窗口等待新数据。</div>
        </section>

        <section className="companion-panel" aria-label="电脑端控制台">
          <div className="console-heading"><div><span className="eyebrow">电脑端控制台</span><h2>账户与设备</h2></div><span className={`app-state-pill ${apiError ? 'warning' : apiState ? 'good' : ''}`}><i />{connectedStatus}</span></div>
          <nav className="panel-tabs" aria-label="电脑端功能">
            {[['accounts', '账户管理'], ['refresh', '刷新设置'], ['setup', '设备配置']].map(([value, label]) => <button type="button" key={value} className={panel === value ? 'active' : ''} aria-pressed={panel === value} onClick={() => setPanel(value)}>{label}</button>)}
          </nav>

          {!apiState && !apiError && <div className="loading-state"><span className="spinner" />正在连接本机应用…</div>}

          {apiState && panel === 'accounts' && <>
            <div className="accounts-heading"><div><h3>本机账户</h3><p>最多 {MAX_ACCOUNTS} 个 · 官方授权或 DeepSeek API 密钥</p></div><button type="button" className="primary" onClick={openProviderDialog} disabled={currentAccountList.length >= MAX_ACCOUNTS || Boolean(busyAction)}>＋ 添加账户</button></div>
            <div className="cli-availability"><span><i className={apiState.cli?.codex ? 'available' : ''} />Codex CLI <strong>{apiState.cli?.codex ? '可用' : '未发现'}</strong></span><span><i className={apiState.cli?.claude ? 'available' : ''} />Claude CLI <strong>{apiState.cli?.claude ? '可用' : '未发现'}</strong></span></div>
            {currentAccountList.length ? <div className="account-list">
              {currentAccountList.map(item => <div className={`account-row ${account?.id === item.id ? 'selected' : ''}`} key={item.id}>
                <button type="button" className="account-select" onClick={() => setSelectedId(item.id)} aria-pressed={account?.id === item.id} aria-label={`选择 ${providerName(item.provider, item.label)} 账户 ${item.provider === 'deepseek' ? '' : item.email || ''}`}>
                  <Logo provider={item.provider} size={34} />
                  <span className="account-detail"><span><strong>{providerName(item.provider, item.label)}</strong><em className={`status-chip ${statusTone(item)}`}>{accountStatus(item)}</em></span><small>{item.provider === 'deepseek' ? 'DeepSeek API · 本机别名' : `${item.email || '邮箱信息未提供'}${item.plan ? ` · ${planLabel(item.plan)}` : ''}`}</small></span>
                  {item.provider === 'deepseek' ? <span className="row-balances"><span>{deepSeekRmbDisplay(item.balance).info?.total_balance ?? '—'}<small>人民币</small></span></span> : <span className="row-quotas">{visibleWindows(item).map(([key, label]) => <span key={key}>{quotaView(item[key], nowSeconds).value === null ? '—' : `${quotaView(item[key], nowSeconds).value}%`}<small>{label}</small></span>)}</span>}
                </button>
                <button type="button" className="remove-account" onClick={() => removeAccount(item.id)} disabled={Boolean(busyAction)} aria-label={`移除 ${item.provider === 'deepseek' ? item.label || 'DeepSeek API' : item.email || '此账户'}`}>移除</button>
              </div>)}
            </div> : <div className="empty-accounts"><div className="empty-icon">＋</div><strong>还没有连接的账户</strong><p>通过官方登录连接 Codex 或 Claude，也可以添加 DeepSeek API 密钥来只读查看账户余额。</p><button type="button" className="primary" onClick={openProviderDialog} disabled={Boolean(busyAction)}>连接第一个账户</button></div>}

            {account && <div className="account-summary">
              <div className="summary-heading"><div><strong>所选账户 · {providerName(account.provider, account.label)}</strong><small>{account.provider === 'deepseek' ? 'DeepSeek API · 本机别名' : account.email || '邮箱信息未提供'}</small></div><span className={`status-chip ${statusTone(account)}`}>{accountStatus(account)}</span></div>
              {account.provider === 'deepseek' ? <DeepSeekBalance balance={account.balance} /> : <SubscriptionQuota account={account} nowSeconds={nowSeconds} stale={selectedCached} />}
              <div className={`freshness ${selectedCached ? 'warning-text' : ''}`}><span className="freshness-dot" />{statusText}{Number.isFinite(account.observed_at) ? ` · ${formatDate(account.observed_at)}` : ''}</div>
              {account.provider === 'deepseek' && <><button type="button" className="secondary deepseek-refresh" onClick={() => refreshAccount(account.id)} disabled={Boolean(busyAction)}>{busyAction.includes(`/accounts/${account.id}/refresh`) ? '正在查询…' : '立即查询余额'}</button><form className="deepseek-key-form" onSubmit={event => updateDeepSeekAccount(event, account.id)}>
                <label className="field"><span>本机别名 <small>{byteLength(accountDeepSeekLabel)} / 32 字节</small></span><input type="text" value={accountDeepSeekLabel} onChange={event => setAccountDeepSeekLabel(event.target.value)} disabled={Boolean(busyAction)} /></label>
                <label className="field"><span>更换 API 密钥 <small>已保存密钥不会显示</small></span><input type="password" autoComplete="new-password" spellCheck="false" value={accountDeepSeekApiKey} onChange={event => setAccountDeepSeekApiKey(event.target.value)} disabled={Boolean(busyAction)} /></label>
                <button type="submit" className="secondary" disabled={Boolean(busyAction) || byteLength(accountDeepSeekLabel) > 32 || (!accountDeepSeekApiKey && accountDeepSeekLabel === (account.label || 'DeepSeek API'))}>保存别名或验证新密钥</button>
                <small>密钥只保存在此电脑的私有账户档案中；更换密钥会先清除旧余额，再验证新的账户。</small>
              </form></>}
              {account.provider !== 'deepseek' && (account.status === 'expired' || (account.status === 'error' && account.authenticated)) && <div className="inline-callout warning-box"><strong>重新连接账户</strong><p>额度保留为最近一次采集结果。需要重新授权时，可在此开始官方登录。</p><button type="button" className="secondary" onClick={() => retryLogin(account.id)} disabled={Boolean(busyAction)}>重新连接</button></div>}
              {account.provider !== 'deepseek' && ((account.status === 'waiting' && !account.authenticated) || (account.status === 'error' && !account.authenticated)) && <div className="inline-callout"><strong>{account.status === 'waiting' ? '等待完成官方登录' : '账户授权需要重试'}</strong><p>按照登录流程打开官方验证页并完成授权。</p><button type="button" className="secondary" onClick={() => retryLogin(account.id)} disabled={Boolean(busyAction)}>继续官方登录</button></div>}
              {selectedPendingLogin && selectedPendingLogin.status === 'pending' && <div className="login-card">
                <div className="login-card-title"><span className="eyebrow">官方登录流程</span><em className="status-chip muted">等待授权</em></div>
                {selectedPendingLogin.code && <div className="verification-code"><span>一次性验证码</span><strong>{selectedPendingLogin.code}</strong><button type="button" className="text-button" onClick={() => copyText(selectedPendingLogin.code)}>复制验证码</button></div>}
                {selectedPendingLogin.url && /^https:\/\//i.test(selectedPendingLogin.url) && <a className="primary login-link" href={selectedPendingLogin.url} target="_blank" rel="noreferrer">打开官方验证页 ↗</a>}
                {selectedPendingLogin.requires_code && <form className="login-code-form" onSubmit={event => { event.preventDefault(); submitLoginCode(account.id); }}><label className="field"><span>官方登录要求输入验证码</span><input type="text" autoComplete="off" spellCheck="false" value={loginCode} onChange={event => setLoginCode(event.target.value)} disabled={Boolean(busyAction)} /></label><button type="submit" className="secondary" disabled={Boolean(busyAction) || !loginCode.trim()}>{busyAction.includes('/login-code') ? '正在提交…' : '提交验证码'}</button><small>验证码只在当前页面内存中短暂保留，不会写入日志或本机存储。</small></form>}
                {!selectedPendingLogin.url && <p className="login-waiting">正在等待官方登录地址…此页面会自动更新。</p>}
                <p className="login-note">官方客户端会引导你完成授权，也可点击下方官方验证链接。本机不会收集或显示账户密码。</p>
              </div>}
              {selectedPendingLogin?.status === 'error' && <div className="inline-callout warning-box"><strong>官方登录流程未完成</strong><p>{friendlyLoginError(selectedPendingLogin.error)}</p><button type="button" className="secondary" onClick={() => retryLogin(account.id)} disabled={Boolean(busyAction)}>重试登录</button></div>}
              {account.provider === 'claude' && <div className="claude-note"><strong>Claude 额度采集说明</strong><p>完成官方授权后，Claude 的额度通常会在此账户首次正常使用后出现。此工具不会自动发送模型请求。</p><button type="button" className="secondary" onClick={() => copyClaudeLaunch(account.id)} disabled={Boolean(busyAction)}>{copiedLaunch ? '已复制启动命令' : '复制官方会话启动命令'}</button><small>命令用于在你自己的 Claude 配置档案中启动官方客户端。</small></div>}
              {account.provider === 'codex' && <p className="source-note">Codex 显示 Codex / CLI 返回的使用窗口；不代表 ChatGPT 普通消息次数或所有 GPT 使用额度。</p>}
            </div>}
          </>}

          {apiState && panel === 'refresh' && <div className="refresh-panel">
            <div className="refresh-title"><div><h3>刷新与显示设置</h3><p>设置账户查询间隔和设备息屏时间</p></div><button type="button" className={`toggle ${autoRefresh ? 'on' : ''}`} role="switch" aria-checked={Boolean(autoRefresh)} aria-label="自动刷新" onClick={() => updateSettings({ refresh_seconds: refreshSeconds, auto_refresh: !autoRefresh }, autoRefresh ? '自动刷新已关闭。' : '自动刷新已开启。')} disabled={Boolean(busyAction)}><span /></button></div>
            <label className="setting-row setting-select"><span><strong>自动刷新间隔</strong><small>仅当自动刷新开启时生效</small></span><select value={refreshSeconds ?? ''} onChange={event => updateSettings({ refresh_seconds: Number(event.target.value), auto_refresh: Boolean(autoRefresh) }, `刷新间隔已设为 ${Number(event.target.value) / 60} 分钟。`)} disabled={!autoRefresh || Boolean(busyAction)}><option value="" disabled>读取设置中</option>{REFRESH_OPTIONS.map(value => <option key={value} value={value}>{value / 60} 分钟</option>)}</select></label>
            <label className="setting-row setting-select"><span><strong>设备息屏时间</strong><small>按键后等待多久关闭屏幕背光</small></span><select value={screenTimeoutSeconds} onChange={event => { const value = Number(event.target.value); updateSettings({ screen_timeout_seconds: value }, `设备息屏时间已设为${value === 0 ? '永不' : ` ${screenTimeoutLabel(value)}`}。`); }} disabled={Boolean(busyAction)}>{SCREEN_TIMEOUT_OPTIONS.map(value => <option key={value} value={value}>{screenTimeoutLabel(value)}</option>)}</select></label>
            <div className="refresh-detail"><div><strong>所选账户</strong><span>{account ? account.provider === 'deepseek' ? account.label || 'DeepSeek API' : account.email || providerName(account.provider) : '未选择账户'}</span></div><div><strong>上次采集</strong><span>{account?.observed_at ? `${formatAge(account.observed_at, nowSeconds)} · ${formatDate(account.observed_at)}` : '尚无采集记录'}</span></div><div><strong>当前状态</strong><span className={selectedCached || (account && account.status !== 'ok') ? 'warning-text' : ''}>{account ? `${accountStatus(account)}${apiError ? ' · 显示缓存' : stale ? ' · 数据较旧' : ''}` : '等待添加账户'}</span></div></div>
            <button type="button" className="primary wide" onClick={refreshAll} disabled={Boolean(busyAction) || !refreshableAccounts.length}>{busyAction.includes('/refresh') ? '正在提交…' : '立即刷新全部账户'}</button>
            <div className="info-note"><strong>额度数据的来源</strong><p>数据由本机程序从已授权的官方客户端、官方账户状态或 DeepSeek 余额接口中采集。DeepSeek 使用余额查询接口，不会发起模型请求，也不会向设备发送 API 密钥。</p><p>Claude 数据可能要等该档案首次正常使用后才会出现。重置时间已到时，页面会等待数据源提供新额度。</p></div>
          </div>}

          {apiState && panel === 'setup' && <div className="setup-panel">
            <div className="setup-title"><div><h3>连接 AI Passport 设备</h3><p>通过 USB 串口把本机 Wi‑Fi 信息发送到设备。</p></div><span className={`device-state ${apiState.device?.enabled ? 'good' : ''}`}><i />{apiState.device?.enabled ? '局域网同步已启用' : '尚未连接设备'}</span></div>
            <div className="setup-gate"><strong>先在设备屏幕上进入「设置 / 配对」</strong><span>设备会在该页面开放最多 120 秒的配置时间。准备好后，再点击连接按钮选择串口。</span></div>
            <div className="setup-current-device">
              <div><span>本机设备地址</span><strong>{apiState.device?.base_url || '未配置'}</strong></div>
              <div><span>设备最近连接</span><strong>{apiState.device?.last_seen ? formatDate(apiState.device.last_seen) : '尚无连接记录'}</strong></div>
              <div><span>局域网地址</span><strong>{apiState.device?.address || '未选择'}</strong></div>
            </div>
            <form className="wifi-form" onSubmit={configureDevice}>
              <label className="field"><span>本机私有 IPv4 地址</span><select value={selectedAddress} onChange={event => setSelectedAddress(event.target.value)} disabled={!addresses.length || serialBusy}><option value="" disabled>{addresses.length ? '选择设备所在网络' : '未发现私有 IPv4 地址'}</option>{addresses.map((item, index) => <option key={`${item.name}-${item.address}-${index}`} value={item.address}>{item.name} · {item.address}</option>)}</select></label>
              <label className="field"><span>Wi‑Fi 名称 <small>{byteLength(wifiSsid)} / 32 字节</small></span><input type="text" autoComplete="off" spellCheck="false" value={wifiSsid} onChange={event => setWifiSsid(event.target.value)} disabled={serialBusy} /></label>
              <label className="field"><span>Wi‑Fi 密码 <small>{byteLength(wifiPassword)} / 64 字节</small></span><input type="password" autoComplete="new-password" value={wifiPassword} onChange={event => setWifiPassword(event.target.value)} disabled={serialBusy} /></label>
              <p className="credential-note"><span className="lock-mark">⌑</span>Wi‑Fi 名称和密码仅用于本次 USB 配置，保留在浏览器内存中，不会发送给本机 API，也不会写入浏览器存储。</p>
              {serialError && <div className="serial-error" role="alert">{serialError}</div>}
              {serialMessage && <div className="serial-success" role="status">{serialMessage}</div>}
              {!navigator.serial?.requestPort && <div className="serial-hint">当前浏览器未提供 Web Serial。请在连接到本机应用的桌面版 Chrome 或 Edge 中操作。</div>}
              <button type="submit" className="primary wide" disabled={serialBusy || Boolean(busyAction) || !addresses.length || !apiState.csrf_token}>{serialBusy ? <><span className="button-spinner" />正在连接设备并配置…</> : '连接设备并配置'}</button>
            </form>
            <div className="setup-footnote"><strong>连接注意</strong><p>配置过程最多等待设备响应 15 秒。完成后串口会关闭，密码会清除。Wi‑Fi 或本机证书 / IP 变化时，请重新进入设备设置页并配对一次。</p></div>
            <div className="lan-controls"><div><strong>局域网同步服务</strong><small>{apiState.device?.enabled ? `当前服务地址：${apiState.device.base_url || '本机'}` : '连接设备后，本机程序会启用安全同步服务。'}</small></div><button type="button" className="secondary" onClick={stopDeviceSync} disabled={!apiState.device?.enabled || Boolean(busyAction)}>停止同步</button></div>
          </div>}
        </section>
      </div>

      <footer className="page-footer"><span>AI Passport 本地电脑伴侣</span><span>账户状态和额度取自本机服务 · 每 5 秒检查一次连接</span></footer>

      {providerDialog && <div className="dialog-backdrop" role="presentation" onMouseDown={event => { if (event.target === event.currentTarget && !busyAction) setProviderDialog(false); }}>
        <section className="connect-dialog" role="dialog" aria-modal="true" aria-labelledby="connect-title">
          <div className="dialog-head"><div><span className="eyebrow">账户连接</span><h2 id="connect-title">连接一个 AI 账户</h2></div><button type="button" className="text-button" onClick={() => { setProviderDialog(false); setNewDeepSeekApiKey(''); }} disabled={Boolean(busyAction)}>关闭</button></div>
          <p className="muted">Codex 与 Claude 使用各自的官方登录流程；DeepSeek 使用只读余额 API。</p>
          <div className="provider-picker" role="group" aria-label="选择服务平台">
            {['codex', 'claude', 'deepseek'].map(provider => <button type="button" key={provider} className={providerChoice === provider ? 'selected' : ''} aria-pressed={providerChoice === provider} onClick={() => { setProviderChoice(provider); setNewDeepSeekApiKey(''); }} disabled={Boolean(busyAction)}><Logo provider={provider} size={31} /><strong>{provider === 'deepseek' ? 'DeepSeek' : providerName(provider)}</strong><small>{provider === 'codex' ? 'OpenAI 官方账户' : provider === 'claude' ? 'Anthropic 官方账户' : '只读余额 API'}</small></button>)}
          </div>
          {providerChoice === 'claude' && <p className="dialog-note">Claude 的额度可能要到该档案首次正常使用后才会显示。此工具不会替你运行模型。</p>}
          {providerChoice === 'deepseek' ? <form className="deepseek-add-form" onSubmit={addDeepSeekAccount}>
            <label className="field"><span>本机别名 <small>{byteLength(newDeepSeekLabel)} / 32 字节 · 不会发送给 DeepSeek</small></span><input type="text" value={newDeepSeekLabel} onChange={event => setNewDeepSeekLabel(event.target.value)} autoComplete="off" disabled={Boolean(busyAction)} /></label>
            <label className="field"><span>DeepSeek API 密钥</span><input type="password" autoComplete="new-password" spellCheck="false" value={newDeepSeekApiKey} onChange={event => setNewDeepSeekApiKey(event.target.value)} required disabled={Boolean(busyAction)} /></label>
            <p className="dialog-note">密钥只保存在此电脑的私有账户档案中，不会显示在账户状态、同步快照或设备上。</p>
            <button type="submit" className="primary wide" disabled={Boolean(busyAction) || !newDeepSeekApiKey || byteLength(newDeepSeekLabel) > 32}>{busyAction === '/api/accounts' ? '正在查询 DeepSeek 余额…' : '添加余额账户'}</button>
          </form> : <>
            <button type="button" className="primary wide" onClick={() => startLogin(providerChoice)} disabled={Boolean(busyAction) || currentAccountList.length >= MAX_ACCOUNTS}>{busyAction === '/api/accounts' ? '正在准备官方登录…' : '继续到官方登录'}</button>
            <p className="dialog-footnote">添加账户不会模拟或预填任何登录信息。官方客户端会引导你完成官方授权。</p>
          </>}
        </section>
      </div>}

      {toast && <div className="toast" role="status">{toast}</div>}
    </main>
  );
}
