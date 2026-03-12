variable "aws_region" {
  description = "AWS region — ap-northeast-1 (Tokyo) for lowest latency to Binance"
  type        = string
  default     = "ap-northeast-1"
}

variable "instance_type" {
  description = "EC2 instance type — c5n for enhanced networking"
  type        = string
  default     = "c5n.2xlarge"
}

variable "key_pair_name" {
  description = "Name of the SSH key pair to use for EC2 access"
  type        = string
}

variable "project_name" {
  description = "Project name prefix for all resources"
  type        = string
  default     = "thames-river-trading"
}

variable "operator_cidr" {
  description = "CIDR block allowed to SSH to the trading instance"
  type        = string
  default     = "0.0.0.0/0"  # Restrict to your IP in production
}
