module "network" {
  source = "./network"

  peered_vpc_id = var.peered_vpc_id
  new_vpc_cidr  = var.tenzir_cidr

  providers = {
    aws               = aws
    aws.monitored_vpc = aws.monitored_vpc
  }
}

module "efs" {
  source            = "./efs"
  count             = var.tenzir_storage_type == "EFS" ? 1 : 0
  name              = "tenzir-server"
  vpc_id            = module.network.new_vpc_id
  subnet_id         = module.network.private_subnet_id
  security_group_id = aws_security_group.server_efs.id
}

module "tenzir_server" {
  source = "./fargate"

  name        = local.tenzir_server_name
  region_name = var.region_name

  vpc_id                      = module.network.new_vpc_id
  subnet_id                   = module.network.private_subnet_id
  security_group_ids          = [aws_security_group.tenzir_server.id, aws_security_group.client_efs.id]
  ecs_cluster_id              = aws_ecs_cluster.fargate_cluster.id
  ecs_cluster_name            = aws_ecs_cluster.fargate_cluster.name
  ecs_task_execution_role_arn = aws_iam_role.fargate_task_execution_role.arn
  service_discov_namespace_id = aws_service_discovery_private_dns_namespace.main.id

  task_cpu    = 2048
  task_memory = 8192

  docker_image = var.tenzir_server_image
  efs = {
    access_point_id = var.tenzir_storage_type == "EFS" ? module.efs[0].access_point_id : ""
    file_system_id  = var.tenzir_storage_type == "EFS" ? module.efs[0].file_system_id : ""
  }
  storage_mount_point = "/var/lib/tenzir"

  entrypoint = "exec tenzir start"
  port       = local.tenzir_port

  environment = [{
    name  = "AWS_REGION"
    value = var.region_name
  }]
}

module "tenzir_client" {
  source = "../common/lambda"

  function_base_name = "client"
  region_name        = var.region_name
  docker_image       = var.lambda_client_image
  memory_size        = 2048
  timeout            = 300

  vpc_id            = module.network.new_vpc_id
  subnets           = [module.network.private_subnet_id]
  security_group_id = aws_security_group.tenzir_client.id

  additional_policies = []
  environment = {
    TENZIR_ENDPOINT = local.tenzir_server_hostname
  }

}
