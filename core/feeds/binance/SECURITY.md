# Security Guidelines for Binance Feed Handler

## API Key Management

The BinanceFeedHandler supports authenticated requests using Binance API keys.

### Loading Credentials

**Method 1: Environment Variables (Recommended)**

```bash
export BINANCE_API_KEY="your_api_key"
export BINANCE_API_SECRET="your_api_secret"
```

```cpp
BinanceFeedHandler handler("BTCUSDT");
```

The handler automatically loads from environment variables.

**Method 2: Direct Parameter (Not Recommended for Production)**

```cpp
BinanceFeedHandler handler("BTCUSDT", "api_key", "api_secret");
```

**Method 3: Configuration File**

Store in `config/dev/secrets.yaml`:
```yaml
binance:
  api_key: "your_key"
  api_secret: "your_secret"
```

Then load and pass to handler.

## Security Best Practices

1. **Never hardcode credentials** in source code
2. **Never commit secrets** to version control
3. **Use separate keys** for dev/shadow/live environments
4. **Restrict permissions** on API keys (read-only for market data)
5. **Rotate keys quarterly**
6. **Set file permissions**: `chmod 600 secrets.yaml`
7. **Never log API keys**, even in debug mode

## Rate Limits

- **Without API Key**: 1200 requests/minute (lower limits)
- **With API Key**: 6000 requests/minute (higher limits)

Public market data endpoints work without authentication but have lower rate limits.

## Production Deployment

For live trading:
1. Use hardware security modules (HSM) for key storage
2. Implement key rotation automation
3. Monitor API key usage via Binance dashboard
4. Set up alerts for suspicious activity
5. Use IP whitelisting on Binance account settings
