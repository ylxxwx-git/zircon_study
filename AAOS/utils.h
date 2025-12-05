#ifndef UTILS_H
#define UTILS_H

void psci_cpu_on(unsigned long core_id, unsigned long entry_point);
int get_core_id();

#endif
