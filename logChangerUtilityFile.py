import sys
import re
global space_count 
import os

#function to find out all the values that we are going to print using that particular log line
def findPrintableVariable(log_line):
	temp = log_line #store the log line in temperary variable 
	temp = ''.join(temp.split())   #remove the extra spaces

	temp = re.sub('".*?"', '', temp)   #remove the " ---- " section from the log line 
  

	for count_position,val in enumerate(temp):
		if val==',':
			break
	temp = temp[11:len(temp)-2]  #remove "ALGO_ILOG" and ");"" from the log
	temp = temp.split(',')   #split the string using ","

	return temp

#to find out the type of all the variable such as %s etc
def findFormatSpecifier(log_line):
	formatSpecifiers = []
	for i in range(0,len(log_line)-1):
		if log_line[i:i+2]=='%s':
			formatSpecifiers.append(log_line[i:i+2])
		#elif log_line[i:i+2]=='%d':
		#	formatSpecifiers.append(log_line[i:i+2])
		#elif log_line[i:i+2]=='%l':
		#	formatSpecifiers.append(log_line[i:i+2])
	return formatSpecifiers

def countSpaceBeforeLog(log_line):
	space_count = 0
	for val in log_line:
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
		elif line[i:i+9]=='ALGO_WLOG':
			log_type = "ALGO_WLOG"
			return log_type
	return log_type

def convertOldLogToNewLog(log_line, specifier,variable,space_count):
	#all the logs should start with TTLOG
	newLog = "TTLOG<<"
	position = 0
	count = 0
	flag = False
	for i in range(10+space_count,len(log_line)):  #log will start from space and len of (ALGO_ILOG)
		if log_line[i]=='"':
			count+=1
		if count==2:    #if we find second ", it means we are done with the double quotes section of the log ALGO_ILOG("-------",-----)
			break
		if log_line[i]=='%':    #if we see some format specifier, save it's type
			flag = True
			newLog = newLog + "<< {0} <<".format(variable[position])
			position+=1
			i = i+2        #ignore the next char to % like %s, we can ignore s, bcs we have already stored the %s
		else:
			if flag==True:
				flag = False
				continue
			if log_line[i] =='"':
				continue
			newLog = newLog + "{0}".format(log_line[i])

    #format the new log into the desired format
	newLog = newLog.split("<<")    

	for i in range(1,len(newLog),2):
		if newLog[i]=="":
			continue
		newLog[i] = '"'+ newLog[i] + '"'

	log = "TTLOG"
	for i in range(1,len(newLog)):
		log = log+ "<<" + newLog[i]

	log = " "*space_count + log+ ";\n"  #add space and ; to the log 

	return log



"""Driver code to the files"""


f = open("original_file.cpp", encoding = 'utf-8')
line = f.readline()

#file where we want to store the result
new_file_name = "file_with_new_log.cpp"	   
if not os.path.isfile('./'+new_file_name):   
		ff = open(new_file_name, "a")
else:
	os.remove(new_file_name)               #if modified file already exists, delete it and redo the process of creating this file
	ff = open(new_file_name, "a")


while line:
	sp = ""
	log_line = ""
	logType = checkLog() # whether this is the log line or not
	c = 0
	
	#log_line = ""  #whenever we will see any log line, it will store the entire log line for further processing
	if len(logType)>0:     #if its a log line, logType length will be greater then 0 (logType = ["ALGO_ILOG","ALGO_ELOG"........])
	    #to handle the logs distributed over multiple lines
		while True:
            #if line is ending with ';', it means it is the end of that log
			if line.endswith(";\n"):           
				log_line = log_line + line
				break
			log_line = log_line +line[:len(line)-1] # append the entire log line except \n
			sys.stdout.flush()
			line = f.readline()	   # do similar task for all the log lines

	if len(logType)>0:  # if some particular line is log line, find out all the values printed in that particular log line
		specifier = findFormatSpecifier(log_line)                   # store all the specifiers in a list [%s, %d, ......]
		variable = findPrintableVariable(log_line)            # find all the values corresponding to all the specifiers and store them in the list
		space_count = countSpaceBeforeLog(log_line)           # count how many spaces are there just before starting of the log so that we can append that many number of logs in the modified log line

		line = convertOldLogToNewLog(log_line, specifier,variable,space_count)   # convert the old log line into the new log format

	ff = open(new_file_name, "a")       # open the file
	ff.write(line)                      #write the line this new file
	ff.close()                          #close the file 

	sys.stdout.flush()
	line = f.readline()                 #repeat this same task for entire file





