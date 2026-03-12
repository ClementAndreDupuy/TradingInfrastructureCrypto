output "instance_public_ip" {
  description = "Elastic IP of the trading instance"
  value       = aws_eip.trading.public_ip
}

output "instance_id" {
  description = "EC2 instance ID"
  value       = aws_instance.trading.id
}

output "s3_bucket_name" {
  description = "S3 bucket for model artifacts"
  value       = aws_s3_bucket.artifacts.bucket
}

output "ssh_command" {
  description = "SSH command to connect to the trading instance"
  value       = "ssh -i ~/.ssh/${var.key_pair_name}.pem ubuntu@${aws_eip.trading.public_ip}"
}
