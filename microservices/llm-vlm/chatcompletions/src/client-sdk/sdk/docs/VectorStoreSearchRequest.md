# VectorStoreSearchRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**query** | [**VectorStoreSearchRequestQuery**](VectorStoreSearchRequestQuery.md) |  | 
**rewrite_query** | **bool** | Whether to rewrite the natural language query for vector search. | [optional] [default to False]
**max_num_results** | **int** | The maximum number of results to return. This number should be between 1 and 50 inclusive. | [optional] [default to 10]
**filters** | [**VectorStoreSearchRequestFilters**](VectorStoreSearchRequestFilters.md) |  | [optional] 
**ranking_options** | [**VectorStoreSearchRequestRankingOptions**](VectorStoreSearchRequestRankingOptions.md) |  | [optional] 

## Example

```python
from openapi_client.models.vector_store_search_request import VectorStoreSearchRequest

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreSearchRequest from a JSON string
vector_store_search_request_instance = VectorStoreSearchRequest.from_json(json)
# print the JSON string representation of the object
print(VectorStoreSearchRequest.to_json())

# convert the object into a dict
vector_store_search_request_dict = vector_store_search_request_instance.to_dict()
# create an instance of VectorStoreSearchRequest from a dict
vector_store_search_request_from_dict = VectorStoreSearchRequest.from_dict(vector_store_search_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


