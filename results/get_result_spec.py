import re
import os
import sys
import subprocess
import glob



replacement_policy = sys.argv[1]

os.system('mkdir -p spec')
output_file='./spec/'+replacement_policy+'.txt'

global num_instructions

workload_list = open("spec_list.txt","rt")
result= open(output_file,'w')
while True:
    workload = workload_list.readline()
    if not workload : break
    workload_temp = workload.split()
    workload_name= replacement_policy + '-' + workload_temp[0]
    result.write(workload_name+' ')
    temp = open(workload_name,"r")
    if temp:
        while True:
            line = temp.readline()
            if not line :
                result.write('\n')
                break
            # Find IPC here
            ipc_find = re.findall(r'Finished',line)
            if ipc_find:
                ipc_temp = line.split()
                # print(ipc_temp)
                ipc = ipc_temp[9]
                num_instructions = int(ipc_temp[4])
                result.write(ipc+' ')
                result.write('%d' %(num_instructions))
            # Find LLC MISS here
            llc_miss_find = re.findall(r'^LLC TOTAL', line)
            if llc_miss_find:
                llc_miss_list = line.split()
                llc_miss = int(llc_miss_list[7])
                # print(llc_miss)
                # print(num_instructions)
                mpki = str(llc_miss/(num_instructions)*1000)
                # print(llc_miss/(num_instructions*1000))
                result.write(' '+mpki)
    temp.close()
workload_list.close()
result.close()


