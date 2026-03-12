terraform {
  required_version = ">= 1.5"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = var.aws_region
}

data "aws_caller_identity" "current" {}

# ── S3 bucket for model artifacts and tick data ───────────────────────────────

resource "aws_s3_bucket" "artifacts" {
  bucket = "${var.project_name}-artifacts-${data.aws_caller_identity.current.account_id}"
  tags   = { Project = var.project_name }
}

resource "aws_s3_bucket_versioning" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id
  versioning_configuration { status = "Enabled" }
}

resource "aws_s3_bucket_lifecycle_configuration" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id
  rule {
    id     = "expire-old-objects"
    status = "Enabled"
    expiration { days = 90 }
  }
}

# ── IAM role for the EC2 instance ────────────────────────────────────────────

resource "aws_iam_role" "trading" {
  name = "${var.project_name}-ec2-role"
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Action    = "sts:AssumeRole"
      Effect    = "Allow"
      Principal = { Service = "ec2.amazonaws.com" }
    }]
  })
}

resource "aws_iam_role_policy" "trading" {
  name = "${var.project_name}-policy"
  role = aws_iam_role.trading.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Effect = "Allow"
        Action = [
          "secretsmanager:GetSecretValue",
          "secretsmanager:DescribeSecret"
        ]
        Resource = "arn:aws:secretsmanager:${var.aws_region}:${data.aws_caller_identity.current.account_id}:secret:trading/*"
      },
      {
        Effect   = "Allow"
        Action   = ["s3:GetObject", "s3:PutObject", "s3:ListBucket"]
        Resource = [
          aws_s3_bucket.artifacts.arn,
          "${aws_s3_bucket.artifacts.arn}/*"
        ]
      },
      {
        Effect = "Allow"
        Action = [
          "cloudwatch:PutMetricData",
          "logs:CreateLogGroup",
          "logs:CreateLogStream",
          "logs:PutLogEvents",
          "logs:DescribeLogStreams"
        ]
        Resource = "*"
      }
    ]
  })
}

resource "aws_iam_instance_profile" "trading" {
  name = "${var.project_name}-profile"
  role = aws_iam_role.trading.name
}

# ── Security group ────────────────────────────────────────────────────────────

resource "aws_security_group" "trading" {
  name        = "${var.project_name}-sg"
  description = "Allow SSH inbound, all outbound"

  ingress {
    description = "SSH"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.operator_cidr]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Project = var.project_name }
}

# ── EC2 instance ──────────────────────────────────────────────────────────────

data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"]  # Canonical
  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd/ubuntu-jammy-22.04-amd64-server-*"]
  }
  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }
}

resource "aws_instance" "trading" {
  ami                    = data.aws_ami.ubuntu.id
  instance_type          = var.instance_type
  key_name               = var.key_pair_name
  iam_instance_profile   = aws_iam_instance_profile.trading.name
  vpc_security_group_ids = [aws_security_group.trading.id]
  availability_zone      = "${var.aws_region}a"

  user_data = templatefile("${path.module}/../userdata.sh", {
    s3_bucket    = aws_s3_bucket.artifacts.bucket
    project_name = var.project_name
  })

  root_block_device {
    volume_size = 50
    volume_type = "gp3"
    encrypted   = true
  }

  tags = {
    Name    = "${var.project_name}-trading-engine"
    Project = var.project_name
  }
}

resource "aws_eip" "trading" {
  instance = aws_instance.trading.id
  domain   = "vpc"
  tags     = { Project = var.project_name }
}

# ── CloudWatch log groups ─────────────────────────────────────────────────────

resource "aws_cloudwatch_log_group" "shadow" {
  name              = "/trading/shadow"
  retention_in_days = 30
}

resource "aws_cloudwatch_log_group" "train" {
  name              = "/trading/train"
  retention_in_days = 30
}

resource "aws_cloudwatch_log_group" "engine" {
  name              = "/trading/engine"
  retention_in_days = 30
}

# ── Secrets Manager placeholder ───────────────────────────────────────────────
# Fill in after terraform apply:
#   aws secretsmanager put-secret-value \
#     --secret-id trading/binance_api_keys \
#     --secret-string '{"api_key":"<KEY>","api_secret":"<SECRET>"}'

resource "aws_secretsmanager_secret" "binance_keys" {
  name                    = "trading/binance_api_keys"
  description             = "Binance API key and secret for SOLUSDT trading"
  recovery_window_in_days = 7
}
