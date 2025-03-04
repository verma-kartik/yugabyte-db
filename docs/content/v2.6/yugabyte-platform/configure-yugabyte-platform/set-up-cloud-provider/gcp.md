---
title: Configure the Google Cloud Platform (GCP) cloud provider
headerTitle: Configure the Google Cloud Platform (GCP) cloud provider
linkTitle: Configure the cloud provider
description: Configure the Google Cloud Platform (GCP) cloud provider.
menu:
  v2.6_yugabyte-platform:
    identifier: set-up-cloud-provider-2-gcp
    parent: configure-yugabyte-platform
    weight: 20
type: docs
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/aws" class="nav-link">
      <i class="fa-brands fa-aws"></i>
      AWS
    </a>
  </li>

  <li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/gcp" class="nav-link active">
      <i class="fa-brands fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/azure" class="nav-link">
      <i class="icon-azure" aria-hidden="true"></i>
      &nbsp;&nbsp; Azure
    </a>
  </li>

  <li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/kubernetes" class="nav-link">
      <i class="fa-solid fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/vmware-tanzu" class="nav-link">
      <i class="fa-solid fa-cubes" aria-hidden="true"></i>
      VMware Tanzu
    </a>
  </li>

<li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/openshift" class="nav-link">
      <i class="fa-solid fa-cubes" aria-hidden="true"></i>OpenShift</a>
  </li>

  <li>
    <a href="/preview/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/on-premises" class="nav-link">
      <i class="fa-solid fa-building"></i>
      On-premises
    </a>
  </li>

</ul>

This page details how to configure Google Cloud Platform (GCP) for YugabyteDB clusters using the Yugabyte Platform console. If no cloud providers are configured yet, the main Dashboard page highlights the need to configure at least one cloud provider.

![Configure Cloud Provider](/images/ee/configure-cloud-provider.png)

## Prerequisites

### Google Cloud Platform (GCP)

If you plan to run YugabyteDB nodes on Google Cloud Platform (GCP), all you need to provide on Yugabyte Platform console is your cloud provider credentials. The Yugabyte Platform will use those credentials to automatically provision and de-provision instances that run YugabyteDB. An 'instance' for YugabyteDB includes a compute instance as well as local or remote disk storage attached to the compute instance.

## Configure GCP

Go to **Configuration** in the navigation bar and then click on the **GCP** tab. You should see
something like this:

![GCP Configuration -- empty](/images/ee/gcp-setup/gcp-configure-empty.png)

Fill in the couple of pieces of data and you should get something like:

![GCP Configuration -- full](/images/ee/gcp-setup/gcp-configure-full.png)

Take note of the following for configuring your GCP provider:

- Give this provider a relevant name. We recommend something that contains Google or GCP in it, especially if you will be configuring other providers as well.

- Upload the JSON file that you obtained when you created your service account previously in [Prepare the Google Cloud Platform (GCP) environment](../../../install-yugabyte-platform/prepare-environment/gcp).

- Assuming this is a new deployment, Yugabyte recommends creating a new VPC specifically for YugabyteDB nodes. You have to ensure that the Yugabyte Platform host machine is able to connect to your Google Cloud account where this new VPC will be created. Otherwise, you can choose to specify an existing VPC for YugabyteDB nodes. The third option that is available only when your Yugabyte Platform host machine is also running on Google Cloud is to use the same VPC that the Yugabyte Platform host machine runs on.

- Finally, click **Save** and give it a couple of minutes, as it will need to do a bit of work in the background. This includes generating a new VPC, a network, subnetworks in all available regions, as well as a new firewall rule, VPC peering for network connectivity and a custom SSH key pair for Yugabyte Platform-to-YugabyteDB connectivity.

Note: Choosing to use the same VPC as Yugabyte Platform is an advanced option, which currently assumes that you are in complete control over this VPC and will be responsible for setting up the networking, SSH access, and firewall rules for it!

The following shows the steps involved in creating this cloud provider.

![GCP Configuration -- in progress](/images/ee/gcp-setup/gcp-configure-inprogress.png)

If all went well, you should see something like:

![GCP Configuration -- success](/images/ee/gcp-setup/gcp-configure-success.png)

Now you are ready to create a YugabyteDB universe on GCP.

## Next step

You are now ready to create YugabyteDB universes as outlined in the next section.
