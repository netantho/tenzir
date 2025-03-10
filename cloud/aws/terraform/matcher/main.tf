provider "aws" {
  region = var.region_name
  default_tags {
    tags = {
      module      = module.env.module_name
      provisioner = "terraform"
      stage       = terraform.workspace
    }
  }
}

resource "aws_cloudwatch_log_group" "fargate_logging" {
  name = "/ecs/gateway/${module.env.module_name}-${local.name}-${module.env.stage}"
}

resource "aws_ecs_task_definition" "fargate_task_def" {
  family                   = "${module.env.module_name}-${local.name}-${module.env.stage}"
  network_mode             = "awsvpc"
  requires_compatibilities = ["FARGATE"]
  cpu                      = local.task_cpu
  memory                   = local.task_memory
  task_role_arn            = aws_iam_role.ecs_task_role.arn
  execution_role_arn       = var.fargate_task_execution_role_arn
  container_definitions    = jsonencode(local.container_definition)
  depends_on               = [aws_cloudwatch_log_group.fargate_logging] // make sure the first task does not fail because log group is not available yet
}

resource "aws_ecs_service" "fargate_service" {
  name                               = "${module.env.module_name}-${local.name}-${module.env.stage}"
  cluster                            = var.fargate_cluster_name
  task_definition                    = aws_ecs_task_definition.fargate_task_def.arn
  desired_count                      = 0
  deployment_maximum_percent         = 100
  deployment_minimum_healthy_percent = 0
  propagate_tags                     = "SERVICE"
  enable_ecs_managed_tags            = true
  launch_type                        = "FARGATE"

  network_configuration {
    subnets          = [var.tenzir_subnet_id]
    security_groups  = [var.tenzir_client_security_group_id]
    assign_public_ip = false
  }

  lifecycle {
    ignore_changes = [desired_count]
  }
}

resource "aws_sqs_queue" "matched_events" {
  name                      = "${module.env.module_name}-${local.name}-${module.env.stage}"
  message_retention_seconds = local.message_retention_seconds
}
