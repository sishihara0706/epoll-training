for i in $(seq 1 100); do
	{
		printf "client-%d\n" "$i"
		sleep $((RANDOM % 10 + 1))
	} | nc 127.0.0.1 8080 &
done

wait
