writeln('CHPL_HOME: ', CHPL_HOME);
writeln('CHPL_TARGET_PLATFORM: ', CHPL_TARGET_PLATFORM);
writeln('CHPL_TARGET_COMPILER: ', CHPL_TARGET_COMPILER);
writeln('CHPL_TARGET_ARCH: ', CHPL_TARGET_ARCH);
writeln('CHPL_TARGET_CPU: ', CHPL_TARGET_CPU);
writeln('CHPL_LOCALE_MODEL: ', CHPL_LOCALE_MODEL);
writeln('CHPL_COMM: ', CHPL_COMM);
if CHPL_COMM == 'gasnet' then {
  writeln('  CHPL_COMM_SUBSTRATE: ', CHPL_COMM_SUBSTRATE);
  if CHPL_COMM == 'gasnet' then
    writeln('  CHPL_GASNET_SEGMENT: ', CHPL_GASNET_SEGMENT);
}
writeln('CHPL_TASKS: ', CHPL_TASKS);
writeln('CHPL_LAUNCHER: ', CHPL_LAUNCHER);
writeln('CHPL_TIMERS: ', CHPL_TIMERS);
writeln('CHPL_UNWIND: ', CHPL_UNWIND);
writeln('CHPL_MEM: ', CHPL_MEM);
writeln('CHPL_ATOMICS: ', CHPL_ATOMICS);
if CHPL_COMM != 'none' then
  writeln('  CHPL_NETWORK_ATOMICS: ', CHPL_NETWORK_ATOMICS);
writeln('CHPL_GMP: ', CHPL_GMP);
writeln('CHPL_HWLOC: ', CHPL_HWLOC);
writeln('CHPL_REGEXP: ', CHPL_REGEXP);
writeln('CHPL_AUX_FILESYS: ', CHPL_AUX_FILESYS);
