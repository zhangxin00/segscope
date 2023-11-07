make

date >> result-10
for((i=0;$i<100;i=$((i+1))))
do
          ./count 10 >> result-10
done
date >> result-10

