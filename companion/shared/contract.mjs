export const MAX_ACCOUNTS = 8;
export const INTERVALS = [60, 300, 900, 1800];
export const SCREEN_TIMEOUT_SECONDS = [0, 30, 60, 120, 300, 600];
export const DEFAULT_SCREEN_TIMEOUT_SECONDS = 120;
export const privateIPv4 = value => typeof value === 'string' && /^(10|192\.168|172\.(1[6-9]|2\d|3[01]))\./.test(value) && value.split('.').length === 4 && value.split('.').every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255);
