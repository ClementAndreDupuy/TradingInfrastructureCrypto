# Configuration

Environment-specific configuration files.

## Directory Structure

- `dev/` - Development environment
- `shadow/` - Shadow trading environment (paper trading)
- `live/` - Live trading environment (real money)

## Secrets Management

**IMPORTANT:** Never commit secrets to git. The `config/live/` directory is gitignored.

### Setup

1. Copy example files:
```bash
cp dev/secrets.yaml.example dev/secrets.yaml
cp dev/secrets.yaml.example shadow/secrets.yaml
cp dev/secrets.yaml.example live/secrets.yaml
```

2. Edit each `secrets.yaml` with your API credentials

3. Set appropriate file permissions:
```bash
chmod 600 dev/secrets.yaml
chmod 600 shadow/secrets.yaml
chmod 600 live/secrets.yaml
```

### Environment Variables

Alternatively, use environment variables:

```bash
export BINANCE_API_KEY="your_key"
export BINANCE_API_SECRET="your_secret"
```

The feed handlers will automatically load from environment variables if no credentials are provided directly.

## API Key Permissions

### Development
- Read-only access
- Market data only
- No trading permissions

### Shadow
- Read-only access
- No actual trading (paper orders)

### Live
- Full trading permissions
- Position limits enforced
- Risk checks enabled

## Key Rotation

Rotate API keys quarterly:
- Generate new keys on exchange
- Update secrets files
- Delete old keys from exchange
- Test with shadow trading first
