#!/bin/bash

# Define the location of the .env file
ENV_FILE="./.env"

# Check if the .env file exists
if [ -f "$ENV_FILE" ]; then
  # Use grep to filter out comments and blank lines, then read each line
  while IFS='=' read -r name value; do
    # Skip lines that are comments or empty
    [[ "$name" =~ ^# ]] && continue
    [[ -z "$name" ]] && continue
    
    # Remove leading/trailing quotes and export the variable
    value=$(echo "$value" | sed -e 's/^"//' -e 's/"$//')
    export "$name"="$value"
  done < <(grep -v '^#' "$ENV_FILE" | grep -v '^$' )
  
  echo "Environment variables loaded from $ENV_FILE"
else
  echo "Warning: $ENV_FILE not found."
fi
