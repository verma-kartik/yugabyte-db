// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import moment from 'moment';
import pluralize from 'pluralize';

import { isDefinedNotNull, isNonEmptyString } from '../../../utils/ObjectUtils';
import {
  getPrimaryCluster,
  getProxyNodeAddress,
  getReadOnlyCluster
} from '../../../utils/UniverseUtils';
import { isNotHidden, isDisabled, isHidden } from '../../../utils/LayoutUtils';
import { YBPanelItem } from '../../panels';
import { NodeAction } from '../../universes';
import { setCookiesFromLocalStorage } from '../../../routes';
import { getUniverseStatus, universeState } from '../helpers/universeHelpers';

import 'react-bootstrap-table/css/react-bootstrap-table.css';

export default class NodeDetailsTable extends Component {
  render() {
    const {
      nodeDetails,
      providerUUID,
      clusterType,
      customer,
      currentUniverse,
      providers
    } = this.props;
    const successIcon = <i className="fa fa-check-circle yb-success-color" />;
    const warningIcon = <i className="fa fa-warning yb-fail-color" />;
    const sortedNodeDetails = nodeDetails.sort((a, b) => a.nodeIdx - b.nodeIdx);
    const universeUUID = currentUniverse.data.universeUUID;
    const providerConfig = providers.data.find((provider) => provider.uuid === providerUUID)
      ?.config;

    const formatIpPort = function (cell, row, type) {
      if (cell === '-') {
        return <span>{cell}</span>;
      }
      const isMaster = type === 'master';
      const href = getProxyNodeAddress(
        universeUUID,
        row.privateIP,
        isMaster ? row.masterPort : row.tserverPort
      );
      const isAlive = isMaster ? row.isMasterAlive : row.isTserverAlive;
      return (
        <div>
          {isAlive ? successIcon : warningIcon}&nbsp;
          {isNotHidden(customer.currentCustomer.data.features, 'universes.proxyIp') ? (
            <a
              href={href}
              onClick={setCookiesFromLocalStorage}
              target="_blank"
              rel="noopener noreferrer"
            >
              {isMaster ? 'Master' : 'TServer'}
            </a>
          ) : (
            <span>{isMaster ? 'Master' : 'TServer'}</span>
          )}
          {isMaster && row.isMasterLeader ? ' (Leader)' : ''}
        </div>
      );
    };

    const getIpPortLinks = (cell, row) => {
      return (
        <Fragment>
          {formatIpPort(row.isMaster, row, 'master')}
          {formatIpPort(row.isTServer, row, 'tserver')}
        </Fragment>
      );
    };

    const getNodeNameLink = (cell, row) => {
      const showIp = isNotHidden(customer.currentCustomer.data.features, 'universes.proxyIp');
      const ip = showIp ? <div className={'text-lightgray'}>{row['privateIP']}</div> : null;
      let nodeName = cell;
      let onPremNodeName = '';
      if (showIp) {
        if (row.cloudInfo.cloud === 'aws') {
          const awsURI = `https://${row.cloudInfo.region}.console.aws.amazon.com/ec2/v2/home?region=${row.cloudInfo.region}#Instances:search=${cell};sort=availabilityZone`;
          nodeName = (
            <a href={awsURI} target="_blank" rel="noopener noreferrer">
              {cell}
            </a>
          );
        } else if (row.cloudInfo.cloud === 'gcp') {
          const gcpURI = `https://console.cloud.google.com/compute/instancesDetail/zones/${row.azItem}/instances/${cell}`;
          nodeName = (
            <a href={gcpURI} target="_blank" rel="noopener noreferrer">
              {cell}
            </a>
          );
        } else if (row.cloudInfo.cloud === 'azu' && isDefinedNotNull(providerConfig)) {
          const tenantId = providerConfig['AZURE_TENANT_ID'];
          const subscriptionId = providerConfig['AZURE_SUBSCRIPTION_ID'];
          const resourceGroup = providerConfig['AZURE_RG'];
          const azuURI = `https://portal.azure.com/#@${tenantId}/resource/subscriptions/${subscriptionId}/resourceGroups/${resourceGroup}/providers/Microsoft.Compute/virtualMachines/${cell}`;
          nodeName = (
            <a href={azuURI} target="_blank" rel="noopener noreferrer">
              {cell}
            </a>
          );
        }
      }

      if (row.cloudInfo.cloud === 'onprem') {
        if (isNonEmptyString(row.instanceName)) {
          onPremNodeName = row.instanceName;
        }
      }

      if (isNonEmptyString(onPremNodeName)) {
        const instanceId = <div className={'text-lightgray'}>{onPremNodeName}</div>;
        return (
          <Fragment>
            {nodeName}
            {ip}
            {instanceId}
          </Fragment>
        );
      } else {
        return (
          <Fragment>
            {nodeName}
            {ip}
          </Fragment>
        );
      }
    };

    const getStatusUptime = (cell, row) => {
      let uptime = '_';
      if (isDefinedNotNull(row.uptime_seconds)) {
        // get the difference between the moments
        const difference = parseFloat(row.uptime_seconds) * 1000;

        //express as a duration
        const diffDuration = moment.duration(difference);
        const diffArray = [
          [diffDuration.years(), 'year'],
          [diffDuration.months(), 'month'],
          [diffDuration.days(), 'day'],
          [diffDuration.hours(), 'hour'],
          [diffDuration.minutes(), 'min'],
          [diffDuration.seconds(), 'sec']
        ];

        /* remove all 0 durations from the array until the first one is found
           not removing all zeros, as we want to display something like 10 min 0 sec -
           so that precision is clear for the user */
        let foundNonZero = false;
        const filteredDurations = diffArray.filter(
          (duration) => foundNonZero === true || (duration[0] !== 0 && (foundNonZero = true))
        );
        if (filteredDurations.length === 1) {
          const diffEntry = filteredDurations[0];
          uptime = `${diffEntry[0]} ${pluralize(diffEntry[1], diffEntry[0])}`;
        } else if (filteredDurations.length > 1) {
          const firstEntry = filteredDurations[0];
          const secondEntry = filteredDurations[1];
          uptime = `${firstEntry[0]} ${pluralize(firstEntry[1], firstEntry[0])}
                    ${secondEntry[0]} ${pluralize(secondEntry[1], secondEntry[0])}`;
        }
        // 0 typically means that the node is in DEAD state - so leave '_' uptime
      }
      return (
        <Fragment>
          <div className={cell === 'Live' ? 'text-green' : 'text-red'}>{cell}</div>
          {uptime}
        </Fragment>
      );
    };

    const getNodeAction = function (cell, row) {
      const hideIP = isHidden(customer.currentCustomer.data.features, 'universes.proxyIp');
      const actions_disabled = isDisabled(
        customer.currentCustomer.data.features,
        'universes.actions'
      );

      const hideQueries =
        !isNotHidden(customer.currentCustomer.data.features, 'universes.details.queries') ||
        row.isTServer === '-';

      if (hideIP) {
        const index = row.allowedActions.indexOf('CONNECT');
        if (index > -1) {
          row.allowedActions.splice(index, 1);
        }
      }

      // get universe provider type to disable STOP and REMOVE actions for kubernetes pods (GH #6084)
      const cluster =
        clusterType === 'primary'
          ? getPrimaryCluster(currentUniverse.data?.universeDetails?.clusters)
          : getReadOnlyCluster(currentUniverse.data?.universeDetails?.clusters);
      const isKubernetes = cluster?.userIntent?.providerType === 'kubernetes';

      return (
        <NodeAction
          currentRow={row}
          universeUUID={universeUUID}
          providerUUID={providerUUID}
          disableStop={isKubernetes}
          disableRemove={isKubernetes}
          hideConnect={hideIP}
          hideQueries={hideQueries}
          disabled={actions_disabled}
          clusterType={clusterType}
        />
      );
    };

    const formatFloatValue = function (cell, row) {
      return cell.toFixed(2);
    };

    const getCloudInfo = function (cell, row) {
      return (
        <Fragment>
          <div>{row.cloudItem}</div>
          {row.regionItem} / {row.azItem}
        </Fragment>
      );
    };

    const getOpsSec = function (cell, row) {
      return (
        <Fragment>
          {isDefinedNotNull(row.read_ops_per_sec) ? formatFloatValue(row.read_ops_per_sec) : '-'} |{' '}
          {isDefinedNotNull(row.write_ops_per_sec) ? formatFloatValue(row.write_ops_per_sec) : '-'}
        </Fragment>
      );
    };

    const getReadableSize = function (cell, row) {
      return isDefinedNotNull(cell) ? parseFloat(cell).toFixed(1) + ' ' + cell.substr(-2, 2) : '-';
    };

    const panelTitle = clusterType === 'primary' ? 'Primary Cluster' : 'Read Replicas';

    const universeStatus = getUniverseStatus(currentUniverse.data);
    const displayNodeActions =
      !this.props.isReadOnlyUniverse &&
      universeStatus.state !== universeState.PAUSED &&
      isNotHidden(customer.currentCustomer.data.features, 'universes.tableActions');

    return (
      <YBPanelItem
        className={`${clusterType}-node-details`}
        header={<h2 className="content-title">{panelTitle}</h2>}
        body={
          <BootstrapTable ref="nodeDetailTable" data={sortedNodeDetails}>
            <TableHeaderColumn
              dataField="name"
              isKey={true}
              className={'node-name-field'}
              columnClassName={'node-name-field'}
              dataFormat={getNodeNameLink}
            >
              Name
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="nodeStatus"
              dataFormat={getStatusUptime}
              className={'yb-node-status-cell'}
              columnClassName={'yb-node-status-cell'}
            >
              Status
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="cloudItem"
              dataFormat={getCloudInfo}
              className="cloud-info-cell"
              columnClassName="cloud-info-cell"
            >
              Cloud Info
            </TableHeaderColumn>
            <TableHeaderColumn dataFormat={getReadableSize} dataField="ram_used">
              RAM Used
            </TableHeaderColumn>
            <TableHeaderColumn dataFormat={getReadableSize} dataField="total_sst_file_size">
              SST Size
            </TableHeaderColumn>
            <TableHeaderColumn dataFormat={getReadableSize} dataField="uncompressed_sst_file_size">
              Uncompressed SST Size
            </TableHeaderColumn>
            <TableHeaderColumn dataFormat={getOpsSec} dataField="read_ops_per_sec">
              Read | Write ops/sec
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="isMaster"
              dataFormat={getIpPortLinks}
              formatExtraData="master"
            >
              Processes
            </TableHeaderColumn>
            {displayNodeActions && (
              <TableHeaderColumn
                dataField="nodeAction"
                className={'yb-actions-cell'}
                columnClassName={'yb-actions-cell'}
                dataFormat={getNodeAction}
              >
                Action
              </TableHeaderColumn>
            )}
          </BootstrapTable>
        }
      />
    );
  }
}
