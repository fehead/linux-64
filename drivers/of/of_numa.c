// SPDX-License-Identifier: GPL-2.0
/*
 * OF NUMA Parsing support.
 *
 * Copyright (C) 2015 - 2016 Cavium Inc.
 */

#define pr_fmt(fmt) "OF: NUMA: " fmt

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/nodemask.h>

#include <asm/numa.h>

/* define default numa node to 0 */
#define DEFAULT_NODE 0

/*
 * Even though we connect cpus to numa domains later in SMP
 * init, we need to know the node ids now for all cpus.
*/
/* IAMROOT20 20240608
 * in	: device tree cpu node
 * out	: 분석된 numa-id를 numa_nodes_parsed 비트맵에 셋팅.
 */
static void __init of_numa_parse_cpu_nodes(void)
{
	u32 nid;
	int r;
	struct device_node *np;

	/* IAMROOT20 20240525
	 * hisilicon/hip07.dtsi 참고
	 */
	for_each_of_cpu_node(np) {
		/* IAMROOT20 20240525
		 * ex)     cpu1: cpu@10001 {
		 *                   device_type = "cpu";
		 *                   compatible = "arm,cortex-a72";
		 *                   reg = <0x10001>;
		 *                   enable-method = "psci";
		 *                   next-level-cache = <&cluster0_l2>;
		 *       ------>     numa-node-id = <0>;
		 *          };
		 */
		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r)
			continue;

		pr_debug("CPU on %u\n", nid);
		if (nid >= MAX_NUMNODES)
			pr_warn("Node id %u exceeds maximum value\n", nid);
		else
			/* IAMROOT20 20240608
			 * numa_nodes_parsed의 bits의 nid번째 비트 필드를 1로 설정
			 */
			node_set(nid, numa_nodes_parsed);
	}
}

/* IAMROOT20 20240608
 * in	: device tree memory node
 * out	: memblock meory영역 별로 numa-id 설정
 */
static int __init of_numa_parse_memory_nodes(void)
{
	struct device_node *np = NULL;
	struct resource rsrc;
	u32 nid;
	int i, r;

	/* IAMROOT20 20240525
	 * ex)    memory@0 {
 	 *                device_type = "memory";
	 *                reg = <0x0 0x00000000 0x0 0x40000000>;
	 *                numa-node-id = <0>;
	 *        };
	 * device_type = "memory"인 모든 노드를 순회
	 * - 'numa-node-id' property 값을 nid에 저장
	 */
	for_each_node_by_type(np, "memory") {
		r = of_property_read_u32(np, "numa-node-id", &nid);
		if (r == -EINVAL)
			/*
			 * property doesn't exist if -EINVAL, continue
			 * looking for more memory nodes with
			 * "numa-node-id" property
			 */
			continue;

		if (nid >= MAX_NUMNODES) {
			pr_warn("Node id %u exceeds maximum value\n", nid);
			r = -EINVAL;
		}

		/* IAMROOT20_END 20240525 */
		/* IAMROOT20 20240615
		 * 매핑된 디바이스의 주소와 정보를 resource 구조체에 넣고,
		 * 기존에 있던 memblock 영역에서, start에서 end영역의 
		 * node id를 nid로 설정한다.
		 */
		for (i = 0; !r && !of_address_to_resource(np, i, &rsrc); i++)
			r = numa_add_memblk(nid, rsrc.start, rsrc.end + 1);

		if (!i || r) {
			of_node_put(np);
			pr_err("bad property in memory node\n");
			return r ? : -EINVAL;
		}
	}

	return 0;
}

static int __init of_numa_parse_distance_map_v1(struct device_node *map)
{
	/* IAMROOT20 20240601
	 * hisilicon/hip07-d05.dts
	 *   distance-map {
	 *	compatible = "numa-distance-map-v1";
	 * 	distance-matrix =
	 *		<0 0 10>, <0 1 15>, <0 2 20>, <0 3 25>,
	 *		<1 0 15>, <1 1 10>, <1 2 25>, <1 3 30>,
	 *		<2 0 20>, <2 1 25>, <2 2 10>, <2 3 15>,
	 *		<3 0 25>, <3 1 30>, <3 2 15>, <3 3 10>;
	 *   };
	 */
	const __be32 *matrix;
	int entry_count;
	int i;

	pr_info("parsing numa-distance-map-v1\n");

	matrix = of_get_property(map, "distance-matrix", NULL);
	if (!matrix) {
		pr_err("No distance-matrix property in distance-map\n");
		return -EINVAL;
	}

	entry_count = of_property_count_u32_elems(map, "distance-matrix");
	if (entry_count <= 0) {
		pr_err("Invalid distance-matrix\n");
		return -EINVAL;
	}

	for (i = 0; i + 2 < entry_count; i += 3) {
		u32 nodea, nodeb, distance;

		nodea = of_read_number(matrix, 1);
		matrix++;
		nodeb = of_read_number(matrix, 1);
		matrix++;
		distance = of_read_number(matrix, 1);
		matrix++;

		if ((nodea == nodeb && distance != LOCAL_DISTANCE) ||
		    (nodea != nodeb && distance <= LOCAL_DISTANCE)) {
			pr_err("Invalid distance[node%d -> node%d] = %d\n",
			       nodea, nodeb, distance);
			return -EINVAL;
		}

		node_set(nodea, numa_nodes_parsed);

		numa_set_distance(nodea, nodeb, distance);

		/* Set default distance of node B->A same as A->B */
		if (nodeb > nodea)
			numa_set_distance(nodeb, nodea, distance);
	}

	return 0;
}

static int __init of_numa_parse_distance_map(void)
{
	int ret = 0;
	struct device_node *np;

	/* IAMROOT20 20240601
	 * hisilicon/hip07-d05.dts
	 *   distance-map {
	 *	compatible = "numa-distance-map-v1";
	 * 	distance-matrix =
	 *		<0 0 10>, <0 1 15>, <0 2 20>, <0 3 25>,
	 *		<1 0 15>, <1 1 10>, <1 2 25>, <1 3 30>,
	 *		<2 0 20>, <2 1 25>, <2 2 10>, <2 3 15>,
	 *		<3 0 25>, <3 1 30>, <3 2 15>, <3 3 10>;
	 *   };
	 */
	np = of_find_compatible_node(NULL, NULL,
				     "numa-distance-map-v1");
	if (np)
		ret = of_numa_parse_distance_map_v1(np);

	of_node_put(np);
	return ret;
}

int of_node_to_nid(struct device_node *device)
{
	struct device_node *np;
	u32 nid;
	int r = -ENODATA;

	np = of_node_get(device);

	while (np) {
		r = of_property_read_u32(np, "numa-node-id", &nid);
		/*
		 * -EINVAL indicates the property was not found, and
		 *  we walk up the tree trying to find a parent with a
		 *  "numa-node-id".  Any other type of error indicates
		 *  a bad device tree and we give up.
		 */
		if (r != -EINVAL)
			break;

		np = of_get_next_parent(np);
	}
	if (np && r)
		pr_warn("Invalid \"numa-node-id\" property in node %pOFn\n",
			np);
	of_node_put(np);

	/*
	 * If numa=off passed on command line, or with a defective
	 * device tree, the nid may not be in the set of possible
	 * nodes.  Check for this case and return NUMA_NO_NODE.
	 */
	if (!r && nid < MAX_NUMNODES && node_possible(nid))
		return nid;

	return NUMA_NO_NODE;
}

int __init of_numa_init(void)
{
	int r;

	of_numa_parse_cpu_nodes();
	/* IAMROOT20_START 20240608 */
	/* IAMROOT20 20240615
	 * memory 영역의 디바이스 노드를 파싱하여
	 * 해당 영역의 memblock에 nid를 설정하고,
	 * numa_distance 배열을 distance_map의 내용으로 초기화한다.
	 */
	r = of_numa_parse_memory_nodes();
	if (r)
		return r;
	return of_numa_parse_distance_map();
}
