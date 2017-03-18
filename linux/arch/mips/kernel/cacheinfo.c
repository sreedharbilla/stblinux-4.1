/*
 * MIPS cacheinfo support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/cacheinfo.h>

/* Populates leaf and increments to next leaf */
#define populate_cache(cache, leaf, c_level, c_type)		\
	leaf->type = c_type;					\
	leaf->level = c_level;					\
	leaf->coherency_line_size = cache.linesz;		\
	leaf->number_of_sets = cache.sets;			\
	leaf->ways_of_associativity = cache.ways;		\
	leaf->size = cache.linesz * cache.sets * cache.ways;

static int __init_cache_level(unsigned int cpu)
{
	struct cpuinfo_mips *c = &current_cpu_data;
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	int levels = 0, leaves = 0;

	/*
	 * If Dcache is not set, we assume the cache structures
	 * are not properly initialized.
	 */
	if (c->dcache.waysize)
		levels += 1;
	else
		return -ENOENT;

	leaves += (c->icache.waysize) ? 2 : 1;

	if (c->scache.waysize) {
		levels++;
		leaves++;
	}

	if (c->tcache.waysize) {
		levels++;
		leaves++;
	}

	this_cpu_ci->num_levels = levels;
	this_cpu_ci->num_leaves = leaves;

	return 0;
}

static int __populate_cache_leaves(unsigned int cpu)
{
	struct cpuinfo_mips *c = &current_cpu_data;
	struct cpu_cacheinfo *this_cpu_ci = get_cpu_cacheinfo(cpu);
	struct cacheinfo *this_leaf = this_cpu_ci->info_list;

	if (c->icache.waysize) {
		populate_cache(c->dcache, this_leaf, 1, CACHE_TYPE_DATA);
		this_leaf++;
		populate_cache(c->icache, this_leaf, 1, CACHE_TYPE_INST);
		this_leaf++;
	} else {
		populate_cache(c->dcache, this_leaf, 1, CACHE_TYPE_UNIFIED);
		this_leaf++;
	}

	if (c->scache.waysize) {
		populate_cache(c->scache, this_leaf, 2, CACHE_TYPE_UNIFIED);
		this_leaf++;
	}

	if (c->tcache.waysize) {
		populate_cache(c->tcache, this_leaf, 3, CACHE_TYPE_UNIFIED);
		this_leaf++;
	}

	return 0;
}

DEFINE_SMP_CALL_CACHE_FUNCTION(init_cache_level)
DEFINE_SMP_CALL_CACHE_FUNCTION(populate_cache_leaves)
