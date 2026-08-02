'use strict';

// Optional Base Sepolia logging layer; only require()'d when ENABLE_BLOCKCHAIN=true, swallows its own errors.

const { ethers } = require('ethers');

// Minimal human-readable ABI — just the one function this module calls.
const CONTRACT_ABI = [
  'function logReading(string deviceId, string alertLevel, int256 heatIndexCx100, uint256 clientTimestamp) external',
];

// Lazily built on first use so require('./ledger') stays side-effect-free
// even with missing env vars; failures surface inside logReading() instead.
let cachedContract = null;

function getContract() {
  if (cachedContract) {
    return cachedContract;
  }

  const rpcUrl = process.env.BASE_SEPOLIA_RPC;
  const contractAddress = process.env.CONTRACT_ADDRESS;
  const privateKey = process.env.SERVER_PRIVATE_KEY;

  if (!rpcUrl || !contractAddress || !privateKey) {
    throw new Error(
      'ledger.js: missing one or more required env vars (BASE_SEPOLIA_RPC, CONTRACT_ADDRESS, SERVER_PRIVATE_KEY)'
    );
  }

  const provider = new ethers.JsonRpcProvider(rpcUrl);
  const wallet = new ethers.Wallet(privateKey, provider);
  cachedContract = new ethers.Contract(contractAddress, CONTRACT_ABI, wallet);
  return cachedContract;
}

// Submit a single reading to the HeatSafetyLedger contract on Base Sepolia; never rejects/throws.
async function logReading(reading) {
  try {
    const contract = getContract();

    const deviceId = String((reading && reading.deviceId) || '');
    const alertLevel = String((reading && reading.alertLevel) || '');

    // Solidity has no decimals: heat index sent as int256 fixed-point x100
    // (e.g. 39.82C -> 3982); signed so sub-zero readings don't underflow.
    const heatIndexC = Number(reading && reading.heatIndexC);
    const heatIndexCx100 = BigInt(Math.round((Number.isFinite(heatIndexC) ? heatIndexC : 0) * 100));

    // Prefer server-stamped receivedAt (ms); fall back to now() if unset.
    const clientTimestamp = BigInt(
      Number.isFinite(reading && reading.receivedAt) ? reading.receivedAt : Date.now()
    );

    const tx = await contract.logReading(deviceId, alertLevel, heatIndexCx100, clientTimestamp);
    console.log(
      `[ledger] submitted logReading tx ${tx.hash} (deviceId=${deviceId}, alertLevel=${alertLevel})`
    );
    return tx.hash;
  } catch (err) {
    // Covers any failure (bad config, RPC, gas, malformed data) — never
    // rethrown, this is a best-effort side channel that must not affect HTTP.
    console.error('[ledger] failed to log reading to Base Sepolia:', err && err.message ? err.message : err);
    return null;
  }
}

module.exports = logReading;
