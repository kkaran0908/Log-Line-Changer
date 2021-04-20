import sys
import re
global space_count 
import os

#function to find out all the values that we are going to print using that particular log line
def findPrintableVariable(d):
	temp = d 
	temp = ''.join(temp.split())
	#for line in d:
	

	temp = re.sub('".*?"', '', temp)

	for count_position,val in enumerate(temp):
		if val==',':
			break

	temp = temp[11:len(temp)-2]  
	temp = temp.split(',')

	return temp

#to find out the type of all the variable such as %s etc
def findSpecifier(d):
	res = []
	for i in range(0,len(d)-1):
		if d[i:i+2]=='%s':
			res.append(d[i:i+2])
	return res

def countSpaceBeforeLog(d):
	space_count = 0
	for val in d:
		if val==" ":
			space_count+=1
		else:
			break
	return space_count

#to check if some particular line is a log line and what kind of log it is info, debug
def checkLog():
	log_type = ""
	for i,val in enumerate(line):
		if val==' ':
			continue
		elif line[i:i+9]=='ALGO_ILOG':
			log_type = "ALGO_ILOG"
			return log_type
		elif line[i:i+9]=='ALGO_ELOG':
			log_type = "ALGO_ELOG"
			return log_type
		elif line[i:i+9]=='ALGO_DLOG':
			log_type = "ALGO_DLOG"
			return log_type
	return log_type

def convertOldLogToNewLog(d, specifier,variable,space_count,logType):
	newLog = "TTLOG<<"
	position = 0
	count = 0
	flag = False
	for i in range(10+space_count,len(d)):
		if d[i]=='"':
			count+=1
		if count==2:
			break
		if d[i]=='%':
			flag = True
			newLog = newLog + "<< {0} <<".format(variable[position])
			position+=1
			i = i+2
		else:
			if flag==True:
				flag = False
				continue
			if d[i] =='"':
				continue
			newLog = newLog + "{0}".format(d[i])

	#print(d)
	#print(newLog)

	newLog = newLog.split("<<")

	#print(newLog)

	for i in range(1,len(newLog),2):
		if newLog[i]=="":
			continue
		newLog[i] = '"'+ newLog[i] + '"'

	#print(newLog)
	log = "TTLOG"
	for i in range(1,len(newLog)):
		log = log+ "<<" + newLog[i]

	log = " "*space_count + log+ ";\n" 

	return log



"""Driver code to handle all the files"""


f = open("original_file.cpp", encoding = 'utf-8')
line = f.readline()

#file where we want to store the result
new_file_name = "file_with_new_log.cpp"	
if not os.path.isfile('./'+new_file_name):
		ff = open(new_file_name, "a")
else:
	os.remove(new_file_name)
	ff = open(new_file_name, "a")


while line:
	sp = ""
	d = ''
	logType = checkLog() # whether this is the log line or not
	c = 0
	#process all the log lines
	d = ""
	if len(logType)>0:
		while True:
            
			if line.endswith(";\n"):
				d = d+line
				break
			d = d+line[:len(line)-1]
			sys.stdout.flush()
			line = f.readline()	

	if len(logType)>0:  # if some particular line is log line, find out all the values printed in that particular log line
		specifier = findSpecifier(d)
		variable = findPrintableVariable(d)
		space_count = countSpaceBeforeLog(d)

		line = convertOldLogToNewLog(d, specifier,variable,space_count,logType)

	ff = open(new_file_name, "a")
	ff.write(line)
	ff.close()

	sys.stdout.flush()
	line = f.readline()





